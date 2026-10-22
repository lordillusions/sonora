#pragma once
// The collab session's view of the app (COL-2..6): AppController implements it in AppCollab.cpp.
#include "app/AppController.h"
#include "io/als/AlsReader.h"

namespace sonora::app
{

class AppController::CollabPort : public collab::CollabSession::Port
{
public:
    explicit CollabPort(AppController& app) : app_(app) {}
    bool guestReady = true; // false while a guest waits for the joined set

    model::Song* song() override { return guestReady ? app_.song_.get() : nullptr; }
    void applyObjects(const std::vector<collab::ObjectState>& states) override { app_.applyCollabObjects(states); }
    void prepareSnapshot() override
    {
        if (! app_.editingDevices_ && ! app_.syncingModel_) app_.syncModelFromEngine();
    }
    std::string resolveSample(const model::FileRef& ref) override { return app_.song_ ? io::resolveSamplePath(*app_.song_, ref) : std::string(); }
    void installSong(std::unique_ptr<model::Song> song) override { app_.installCollabSong(std::move(song)); }
    void assetsArrived(const std::vector<std::string>& hashes) override { app_.collabAssetsArrived(hashes); }
    void applyLive(const std::vector<collab::LiveValue>& values) override { app_.applyLiveValues(values); }
    void applyPluginState(const collab::PluginStateMsg& m) override { app_.applyCollabPluginState(m); }
    void applyTransport(const collab::TransportMsg& m, int64_t localTimeMs) override { app_.applyCollabTransport(m, localTimeMs); }
    collab::TransportState transportState() override
    {
        return {app_.engine_.transport().isPlaying(), app_.engine_.transport().positionBeats()};
    }
    std::string performDrop(collab::AuthorId author, const juce::File& file, const collab::DropRequestMsg& m) override
    {
        return app_.performCollabDrop(author, file, m);
    }
    void changed() override { app_.refreshCollabView(); }
    void notify(const std::string& text) override { app_.collabNotice(text); }
    void ended(const std::string& reason) override { app_.collabEnded(reason); }

private:
    AppController& app_;
};

} // namespace sonora::app
