// COL-2..6: collab sessions in the app (docs/COLLAB_DESIGN.md, ADR 0009/0010).
//
// AppController owns at most one CollabSession. Its Port applies remote changes through the same engine/UI refresh as
// local edits (afterEdit), keeps live values (mixer, device parameters, bypass) in step with a 30 Hz poll of the
// engine, mirrors plugin state changed inside plugin editors, and plays the shared transport at the scheduled time.
// Local edits still run through applyEdit; the session turns them into object patches afterwards.
#include "app/AppController.h"
#include "app/CollabPort.h"

#include "collab/Objects.h"
#include "core/ModelCodec.h"
#include "core/Sha256.h"
#include "devices/DeviceFactory.h"
#include "devices/RackProcessor.h"
#include "io/als/AlsReader.h"
#include "platform/MediaAccess.h"
#include "platform/SecretStore.h"
#include "plugins/PluginDevice.h"
#include "ui/SetView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <string_view>

namespace sonora::app
{
namespace
{
#ifndef SONORA_COLLAB_DEFAULT_RELAY
#define SONORA_COLLAB_DEFAULT_RELAY ""
#endif
// COL-3b: the project's relay (CMake SONORA_COLLAB_RELAY, https://relay.sonoradaw.com); empty = a relay on this Mac.
const char* defaultRelay()
{
    if constexpr (std::string_view(SONORA_COLLAB_DEFAULT_RELAY).empty()) return "http://127.0.0.1:8787";
    else return SONORA_COLLAB_DEFAULT_RELAY;
}
const platform::SecretId kRelayKey{"Sonora Collab Relay", "host-token"};

ui::CollabView::State viewState(collab::CollabSession::State s)
{
    using S = collab::CollabSession::State;
    switch (s)
    {
        case S::Starting: return ui::CollabView::State::Starting;
        case S::Joining: return ui::CollabView::State::Joining;
        case S::Active: return ui::CollabView::State::Active;
        case S::Reconnecting: return ui::CollabView::State::Reconnecting;
        case S::Ended: return ui::CollabView::State::Ended;
    }
    return ui::CollabView::State::None;
}

ui::CollabView::Role viewRole(collab::Role r)
{
    return r == collab::Role::Host ? ui::CollabView::Role::Host : r == collab::Role::Listener ? ui::CollabView::Role::Listener : ui::CollabView::Role::Editor;
}

// Calls fn(device, processor) for a track's device chain and every rack child, model and engine side by side.
template <class F> void forEachDeviceProcessor(const std::vector<model::Device>& devices, const std::vector<devices::DeviceProcessor*>& processors, F&& fn)
{
    for (size_t i = 0; i < devices.size() && i < processors.size(); ++i)
    {
        if (! processors[i]) continue;
        fn(devices[i], processors[i]);
        if (const auto* rack = dynamic_cast<devices::RackProcessor*>(processors[i]))
            for (size_t c = 0; c < devices[i].chains.size() && c < rack->chains().size(); ++c)
                forEachDeviceProcessor(devices[i].chains[c].devices, rack->chains()[c].processorForModelDevice, fn);
    }
}
} // namespace

void AppController::initialiseCollab()
{
    collabPort_ = std::make_unique<CollabPort>(*this);
    ui_.collab.networkAvailable = collab::networkAvailable();
    ui_.collab.displayName = collabName();
    // Before COL-3b the local-relay address was the default and got saved with the other settings; it reached nobody
    // over the internet. Forget it once so "Share" uses the project's server (a developer can set it again).
    if (! settings().getBoolValue("collabRelayMigrated") && settings().getValue("collabRelay") == "http://127.0.0.1:8787"
        && juce::String(defaultRelay()) != "http://127.0.0.1:8787")
        settings().removeValue("collabRelay");
    settings().setValue("collabRelayMigrated", true);
    ui_.collab.relayUrl = settings().getValue("collabRelay", defaultRelay()).toStdString();
    ui_.collab.relayIsDefault = ui_.collab.relayUrl == defaultRelay();
    // COL-9: what the next session allows; the host's latest choice is kept as the default.
    ui_.collab.policy.chat = settings().getBoolValue("collabAllowChat", true);
    ui_.collab.policy.voice = settings().getBoolValue("collabAllowVoice", true);
    ui_.collab.policy.video = settings().getBoolValue("collabAllowVideo", true);
    ui_.collab.voiceSupported = (collab::supportedFeatures & collab::FeatureVoice) != 0;
    ui_.collab.videoSupported = (collab::supportedFeatures & collab::FeatureVideo) != 0;
    ui_.collabSetPolicy = [this](ui::CollabView::Policy p) {
        if (collab_ && inCollabSession() && ! collab_->isHost()) return;
        settings().setValue("collabAllowChat", p.chat);
        settings().setValue("collabAllowVoice", p.voice);
        settings().setValue("collabAllowVideo", p.video);
        settings().saveIfNeeded();
        ui_.collab.policy = p;
        if (collab_ && inCollabSession())
        {
            collab_->setPolicy({p.chat, p.voice, p.video});
            collabNotice(p.chat ? "Chat is on for everyone in the session" : "Chat is off for everyone in the session");
        }
        refreshCollabView();
    };
    // COL-7: voice options are this person's; the mic always starts off.
    ui_.collab.pushToTalk = settings().getBoolValue("collabPushToTalk", false);
    engine_.voiceTap().testToneHz = juce::SystemStats::getEnvironmentVariable("SONORA_COLLAB_VOICE_TONE", {}).getFloatValue();
    ui_.collab.duck = settings().getBoolValue("collabDuck", true);
    ui_.collabSetMic = [this](bool on) { setCollabMic(on); };
    ui_.collabSetVoiceOption = [this](int option, bool on) {
        if (option == 0) ui_.collab.pushToTalk = on;
        else ui_.collab.duck = on;
        settings().setValue(option == 0 ? "collabPushToTalk" : "collabDuck", on);
        settings().saveIfNeeded();
        if (option == 0 && on) collabNotice("Push to talk: hold the ` key (left of 1) while you speak");
        pollVoice();
        refreshCollabView();
    };
    ui_.collabMuteVoice = [this](int id, bool mute) {
        const auto who = static_cast<collab::AuthorId>(id);
        if (mute) mutedVoices_.insert(who);
        else mutedVoices_.erase(who);
        if (voice_) voice_->setMuted(who, mute);
        refreshCollabView();
    };
    ui_.collabPushToTalkKey = [this](bool down) {
        pushToTalkHeld_ = down;
        ui_.collab.pushToTalkHeld = down;
        pollVoice();
        refreshCollabView();
    };
    // COL-8: the camera always starts off; whether the tiles show is this machine's choice.
    ui_.collab.showVideo = settings().getBoolValue("collabShowVideo", true);
    ui_.collabSetCamera = [this](bool on) { setCollabCamera(on); };
    ui_.collabShowVideo = [this](bool show) {
        ui_.collab.showVideo = show;
        settings().setValue("collabShowVideo", show);
        settings().saveIfNeeded();
        if (! show) collabNotice("Video tiles hidden. Show them again in the Collaborate panel.");
        refreshCollabView();
    };
    ui_.collabShare = [this](bool lan) { shareSession(lan); };
    ui_.collabJoinLink = [this] { joinSessionDialog(); };
    ui_.collabJoinNearby = [this](const std::string& address) { joinNearbySession(address); };
    ui_.collabLeave = [this] { leaveSession(); };
    ui_.collabCopyInvite = [this] {
        if (! collab_) return;
        juce::SystemClipboard::copyTextToClipboard(juce::String::fromUTF8(collab_->inviteLink().c_str()));
        collabNotice("Invite link copied. Send it to the people you want to work with.");
    };
    ui_.collabEditName = [this] {
        auto* dialog = new juce::AlertWindow("Your name", "How others see you in a session", juce::MessageBoxIconType::NoIcon);
        dialog->addTextEditor("name", juce::String::fromUTF8(collabName().c_str()));
        dialog->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        dialog->enterModalState(true, juce::ModalCallbackFunction::create([this, alive = std::weak_ptr<bool>(alive_), dialog](int result) {
            if (alive.expired() || result != 1) return;
            const auto name = dialog->getTextEditorContents("name").trim().substring(0, 64);
            if (name.isEmpty()) return;
            settings().setValue("collabName", name);
            settings().saveIfNeeded();
            ui_.collab.displayName = name.toStdString();
            if (ui_.requestRepaint) ui_.requestRepaint();
        }), true);
    };
    ui_.collabEditRelay = [this] {
        auto* dialog = new juce::AlertWindow("Connection Settings",
                                             "Sharing over the internet goes through a server that introduces you to each other; the music "
                                             "itself goes directly between you, encrypted. Leave the address empty to use Sonora's server. "
                                             "The access key is only needed to host on servers that ask for one.",
                                             juce::MessageBoxIconType::NoIcon);
        dialog->addTextEditor("relay", ui_.collab.relayIsDefault ? juce::String() : juce::String::fromUTF8(ui_.collab.relayUrl.c_str()),
                              "Server address (empty = " + juce::String(defaultRelay()) + ")");
        juce::String key;
        platform::SecretStore().read(kRelayKey, key);
        dialog->addTextEditor("key", key, "Access key (optional)", true);
        dialog->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        dialog->enterModalState(true, juce::ModalCallbackFunction::create([this, alive = std::weak_ptr<bool>(alive_), dialog](int result) {
            if (alive.expired() || result != 1) return;
            auto relay = dialog->getTextEditorContents("relay").trim();
            while (relay.endsWith("/")) relay = relay.dropLastCharacters(1);
            if (relay.isEmpty()) relay = defaultRelay();
            if (! relay.startsWith("https://") && ! relay.startsWith("http://127.0.0.1") && ! relay.startsWith("http://localhost"))
            {
                collabNotice("The server address must start with https:// (plain http only for a server on this Mac).");
                return;
            }
            if (relay == defaultRelay()) settings().removeValue("collabRelay");
            else settings().setValue("collabRelay", relay);
            settings().saveIfNeeded();
            ui_.collab.relayUrl = relay.toStdString();
            ui_.collab.relayIsDefault = relay == defaultRelay();
            const auto accessKey = dialog->getTextEditorContents("key").trim();
            platform::SecretStore store;
            if (accessKey.isEmpty()) store.remove(kRelayKey);
            else store.write(kRelayKey, accessKey);
            if (ui_.requestRepaint) ui_.requestRepaint();
        }), true);
    };
    ui_.collabSetRole = [this](int id, ui::CollabView::Role role) {
        if (collab_) collab_->setRole(static_cast<collab::AuthorId>(id), role == ui::CollabView::Role::Listener ? collab::Role::Listener : collab::Role::Editor);
    };
    ui_.collabRemove = [this](int id) { if (collab_) collab_->kick(static_cast<collab::AuthorId>(id)); };
    ui_.collabAnswerJoin = [this](const std::string& peer, bool allow) { if (collab_) collab_->approveJoin(peer, allow); };
    ui_.collabSetFollowTransport = [this](bool on) {
        if (collab_) collab_->setFollowTransport(on);
        ui_.collab.followTransport = on;
    };
    ui_.collabPointer = [this](int view, int trackIndex, double beat, int pitch) {
        if (! collab_ || collab_->state() != collab::CollabSession::State::Active || ! song_) return;
        collab::PresenceMsg p;
        p.view = static_cast<collab::PresenceMsg::View>(std::clamp(view, 0, 2));
        if (const auto* t = ui_.trackAt(trackIndex)) p.trackId = t->id;
        p.beat = beat;
        p.pitch = pitch;
        if (const auto* clip = ui_.selectedClip())
        {
            p.selectedClipUid = clip->uid;
            if (view == 2) p.clipUid = clip->uid;
        }
        const auto indices = ui_.selectedTrackIndices();
        if (! indices.empty())
        {
            if (const auto* a = ui_.trackAt(indices.front())) p.selectedTrackFrom = a->id;
            if (const auto* b = ui_.trackAt(indices.back())) p.selectedTrackTo = b->id;
        }
        if (ui_.arrangementSelection.active)
        {
            p.selectionStart = ui_.arrangementSelection.start;
            p.selectionEnd = ui_.arrangementSelection.end;
        }
        collab_->sendPresence(p);
    };
    collabTimer_.startTimer(10);
    // Scripted check of two real instances (docs/collab/TESTING.md): SONORA_COLLAB_AUTOTEST=host:<file> or join:<file>.
    const auto autotest = juce::SystemStats::getEnvironmentVariable("SONORA_COLLAB_AUTOTEST", {}).toStdString();
    if (! autotest.empty())
        juce::Timer::callAfterDelay(1500, [this, alive = std::weak_ptr<bool>(alive_), autotest] {
            if (! alive.expired()) startCollabAutotest(autotest);
        });
}

// Each instance makes one edit through the ordinary applyEdit path once the session runs, then logs a hash of every
// replicated object, so a script can compare host and guest without touching the UI.
void AppController::startCollabAutotest(const std::string& spec)
{
    const auto colon = spec.find(':');
    const auto role = spec.substr(0, colon);
    const juce::File file(juce::String::fromUTF8(colon == std::string::npos ? "" : spec.substr(colon + 1).c_str()));
    auto log = [](const std::string& text) { std::fprintf(stderr, "[sonora] collab autotest: %s\n", text.c_str()); };
    collabAutotest_ = true;
    auto report = [this, log] {
        if (! song_) return;
        std::string all = model::codec::encodeDocument(*song_);
        for (const auto& key : collab::objectKeys(*song_)) all += collab::toString(key) + *collab::encodeObject(*song_, key);
        const auto digest = crypto::toHex(crypto::sha256(all));
        log("state tempo=" + std::to_string(song_->tempo) + " track0=" + (song_->tracks.empty() ? std::string() : song_->tracks[0].name)
            + " seq=" + std::to_string(collab_ ? collab_->seq() : 0) + " people=" + std::to_string(collab_ ? collab_->participants().size() : 0)
            + " chat=" + std::to_string(collab_ ? collab_->chat().size() : 0) + " hash=" + digest.substr(0, 16));
        if (voice_)
        {
            const auto st = voice_->stats();
            log("voice sent=" + std::to_string(st.sent) + " received=" + std::to_string(st.received) + " decoded=" + std::to_string(st.decoded)
                + " concealed=" + std::to_string(st.concealed) + " late=" + std::to_string(st.late) + " underruns="
                + std::to_string(engine_.voiceTap().underruns()) + " media=" + (collab_ && collab_->voiceAvailable() ? "ok" : "off"));
        }
        if (video_)
        {
            const auto st = video_->stats();
            size_t tiles = 0;
            for (const auto& t : ui_.collab.videoTiles) tiles += t.self ? 0 : 1;
            log("video framesSent=" + std::to_string(st.framesSent) + " packetsSent=" + std::to_string(st.packetsSent) + " packetsReceived="
                + std::to_string(st.packetsReceived) + " decoded=" + std::to_string(st.framesDecoded) + " dropped=" + std::to_string(st.framesDropped)
                + " remoteTiles=" + std::to_string(tiles) + " relayed=" + (collab_ && ! collab_->videoAvailable() ? collab_->videoUnavailableReason() : "no"));
        }
    };
    if (role == "host")
    {
        shareSession(true);
        auto poll = std::make_shared<std::function<void(int)>>();
        *poll = [this, file, log, report, poll, alive = std::weak_ptr<bool>(alive_)](int step) {
            if (alive.expired() || ! collab_) return;
            if (step == 0 && collab_->inviteLink().empty()) { juce::Timer::callAfterDelay(200, [poll] { (*poll)(0); }); return; }
            if (step == 0)
            {
                file.replaceWithText(juce::String::fromUTF8(collab_->inviteLink().c_str()), false, false, "\n");
                log("invite written");
                juce::Timer::callAfterDelay(200, [poll] { (*poll)(1); });
                return;
            }
            if (step == 1 && collab_->participants().size() < 2) { juce::Timer::callAfterDelay(200, [poll] { (*poll)(1); }); return; }
            if (step == 1)
            {
                log("guest joined");
                juce::Timer::callAfterDelay(1500, [this, poll, log] {
                    applyEdit(model::setTempo(*song_, 111.0));
                    log("host set tempo 111");
                    const auto why = sendCollabChat(juce::String::fromUTF8("hello from the host \xf0\x9f\x8e\xb9").toStdString());
                    log(why.empty() ? "host sent a chat line" : "host chat refused: " + why);
                    if (juce::SystemStats::getEnvironmentVariable("SONORA_COLLAB_VIDEO_TEST", {}).getIntValue() > 0)
                    {
                        setCollabCamera(true); // a moving test pattern instead of the camera
                        log(collabCameraOn_ ? "host camera on (test pattern)" : "host camera refused: " + collab_->videoUnavailableReason());
                    }
                    if (engine_.voiceTap().testToneHz.load() > 0.0f)
                    {
                        setCollabMic(true); // SONORA_COLLAB_VOICE_TONE: the host talks with a test tone
                        log(collabMicOn_ ? "host mic on (test tone)" : "host mic refused: " + collab_->voiceUnavailableReason());
                    }
                    juce::Timer::callAfterDelay(4000, [poll] { (*poll)(2); });
                });
                return;
            }
            report();
            juce::Timer::callAfterDelay(2000, [poll] { (*poll)(2); });
        };
        (*poll)(0);
        return;
    }
    if (role == "join")
    {
        auto poll = std::make_shared<std::function<void(int)>>();
        *poll = [this, file, log, report, poll, alive = std::weak_ptr<bool>(alive_)](int step) {
            if (alive.expired()) return;
            if (step == 0)
            {
                const auto link = file.loadFileAsString().trim();
                if (link.isEmpty()) { juce::Timer::callAfterDelay(200, [poll] { (*poll)(0); }); return; }
                collabNotice("autotest joining");
                std::string error;
                const auto invite = collab::parseInvite(link.toStdString(), error);
                if (! invite) { log("bad invite: " + error); return; }
                const auto name = std::string("Autotest Guest");
                collabPort_->guestReady = false;
                collab::CollabSession::Options options;
                options.name = name;
                startCollab(collab::CollabSession::guest(*collabPort_, collab::openGuest(*invite, name), options));
                juce::Timer::callAfterDelay(200, [poll] { (*poll)(1); });
                return;
            }
            if (step == 1 && (! collab_ || collab_->state() != collab::CollabSession::State::Active || collab_->downloadsPending() > 0))
            {
                if (collab_ && collab_->state() == collab::CollabSession::State::Ended) { log("ended: " + collab_->status()); return; }
                juce::Timer::callAfterDelay(200, [poll] { (*poll)(1); });
                return;
            }
            if (step == 1)
            {
                log("joined");
                juce::Timer::callAfterDelay(500, [this, poll, log] {
                    if (! song_->tracks.empty()) applyEdit(model::renameTrack(*song_, 0, "Renamed by guest"));
                    log("guest renamed track 0");
                    const auto why = sendCollabChat("hello from the guest");
                    log(why.empty() ? "guest sent a chat line" : "guest chat refused: " + why);
                    juce::Timer::callAfterDelay(5000, [poll] { (*poll)(2); });
                });
                return;
            }
            report();
            juce::Timer::callAfterDelay(2000, [poll] { (*poll)(2); });
        };
        (*poll)(0);
    }
}

namespace
{
std::string appVersion()
{
    if (auto* app = juce::JUCEApplicationBase::getInstance()) return app->getApplicationVersion().toStdString();
    return {};
}
} // namespace

std::string AppController::collabName()
{
    auto name = settings().getValue("collabName").trim();
    if (name.isEmpty()) name = juce::SystemStats::getFullUserName().trim();
    if (name.isEmpty()) name = juce::SystemStats::getLogonName();
    return name.substring(0, 64).toStdString();
}

// ------------------------------------------------------------------------------------------------ starting and ending

void AppController::startCollab(std::unique_ptr<collab::CollabSession> session)
{
    collab_ = std::move(session);
    // Play, stop and seeks during playback are shared: everyone starts at the same scheduled moment.
    engine_.transport().requestHook = [this](engine::Transport::Request request, double beat) {
        if (! collab_ || collab_->state() != collab::CollabSession::State::Active || ! collab_->followTransport()) return false;
        using R = engine::Transport::Request;
        collab_->requestTransport(request == R::Play ? collab::TransportMsg::Action::Play
                                  : request == R::Stop ? collab::TransportMsg::Action::Stop : collab::TransportMsg::Action::Seek, beat);
        return true;
    };
    liveSent_.clear();
    pluginStateHash_.clear();
    collabTicks_ = 0;
    ui_.collab.followTransport = true;
    ui_.collab.unread = 0;
    collabMicOn_ = false;
    pushToTalkHeld_ = false;
    mutedVoices_.clear();
    collabCameraOn_ = false;
    ui_.collab.notices.clear();
    ui_.collab.endedReason.clear();
    ui_.collabOpen = true;
    refreshCollabView();
}

void AppController::shareSession(bool lan)
{
    if (inCollabSession()) { ui_.collabOpen = true; return; }
    if (! song_) { collabNotice("Open or create a set first, then share it."); return; }
    if (! collab::networkAvailable()) { collabNotice("This build of Sonora has no collab networking."); return; }
    if (ui_.recording || exporting_) { collabNotice("Finish recording or exporting before sharing."); return; }
    collab::HostNetOptions net;
    net.mode = lan ? collab::HostNetOptions::Mode::Lan : collab::HostNetOptions::Mode::Relay;
    net.relay = ui_.collab.relayUrl;
    juce::String key;
    if (! lan && platform::SecretStore().read(kRelayKey, key).ok()) net.hostToken = key.toStdString();
    net.displayName = collabName();
    const juce::File source(juce::String::fromUTF8(song_->sourcePath.c_str()));
    net.sessionName = isUntitled() ? "Untitled" : source.getFileNameWithoutExtension().toStdString();
    auto transport = collab::openHost(net);
    if (! transport) { collabNotice("Could not start sharing."); return; }
    collab::CollabSession::Options options;
    options.name = collabName();
    options.policy = {ui_.collab.policy.chat, ui_.collab.policy.voice, ui_.collab.policy.video};
    options.sessionName = net.sessionName;
    options.appVersion = appVersion();
    options.projectDirectory = source.getParentDirectory();
    collabPort_->guestReady = true;
    startCollab(collab::CollabSession::host(*collabPort_, std::move(transport), options));
    ui_.collab.sessionName = net.sessionName;
    copyInviteWhenReady_ = ! lan;
    collabNotice(lan ? "Sharing on your Wi-Fi. Send the invite link, or people on the same network pick you from their Join list."
                     : "Sharing over the internet. The invite link is copied as soon as it is ready.");
}

void AppController::joinSessionDialog()
{
    if (inCollabSession()) { ui_.collabOpen = true; return; }
    auto* dialog = new juce::AlertWindow("Join Session", "Paste the invite link you were sent.", juce::MessageBoxIconType::NoIcon);
    const auto clip = juce::SystemClipboard::getTextFromClipboard().trim();
    dialog->addTextEditor("link", clip.startsWith("sonora://join/") ? clip : juce::String(), "Invite link");
    dialog->addTextEditor("name", juce::String::fromUTF8(collabName().c_str()), "Your name");
    dialog->addButton("Join", 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([this, alive = std::weak_ptr<bool>(alive_), dialog](int result) {
        if (alive.expired() || result != 1) return;
        const auto name = dialog->getTextEditorContents("name").trim().substring(0, 64);
        if (name.isNotEmpty() && name.toStdString() != collabName())
        {
            settings().setValue("collabName", name);
            settings().saveIfNeeded();
            ui_.collab.displayName = name.toStdString();
        }
        joinSession(dialog->getTextEditorContents("link").trim().toStdString());
    }), true);
}

void AppController::joinSession(const std::string& link)
{
    if (inCollabSession()) return;
    std::string error;
    const auto invite = collab::parseInvite(link, error);
    if (! invite) { collabNotice(error); return; }
    if (! collab::networkAvailable()) { collabNotice("This build of Sonora has no collab networking."); return; }
    confirmDiscardChanges([this, accepted = *invite] {
        const auto name = collabName();
        auto transport = collab::openGuest(accepted, name);
        if (! transport) return;
        collab::CollabSession::Options options;
        options.name = name;
        options.appVersion = appVersion();
        collabPort_->guestReady = false;
        startCollab(collab::CollabSession::guest(*collabPort_, std::move(transport), options,
                                                 [accepted, name] { return collab::openGuest(accepted, name); }));
    });
}

void AppController::joinNearbySession(const std::string& address)
{
    if (inCollabSession() || address.empty()) return;
    confirmDiscardChanges([this, address] {
        const auto name = collabName();
        auto transport = collab::openGuestByAddress(address, name);
        if (! transport) return;
        collab::CollabSession::Options options;
        options.name = name;
        options.appVersion = appVersion();
        collabPort_->guestReady = false;
        startCollab(collab::CollabSession::guest(*collabPort_, std::move(transport), options));
    });
}

void AppController::leaveSession()
{
    if (! collab_) return;
    if (collab_->state() == collab::CollabSession::State::Ended)
    {
        endCollabDocument();
        return;
    }
    collab_->leave();
}

void AppController::collabEnded(const std::string& reason)
{
    engine_.transport().requestHook = nullptr;
    voice_.reset();
    video_.reset(); // the camera light goes off with the session
    collabMicOn_ = false;
    collabCameraOn_ = false;
    ui_.collab.videoTiles.clear();
    ui_.collab.endedReason = reason;
    collabNotice(reason);
    // Remote changes were interleaved with this person's own: the local history no longer matches the song.
    history_.clear();
    history_.markExternalChange();
    if (collabGuest() && song_)
        collabNotice("You can keep working on this copy; File > Save a Copy writes it with its samples.");
    refreshCollabView();
}

void AppController::endCollabDocument()
{
    engine_.transport().requestHook = nullptr;
    voice_.reset();
    video_.reset();
    collabMicOn_ = false;
    collabCameraOn_ = false;
    ui_.collab.videoTiles.clear();
    collab_.reset(); // a guest's sample resolver goes with it
    collabPort_->guestReady = true;
    ui_.collab.state = ui::CollabView::State::None;
    ui_.collab.participants.clear();
    ui_.collab.cursors.clear();
    ui_.collab.joinRequests.clear();
    ui_.collab.invite.clear();
    ui_.collab.status.clear();
    ui_.collab.progress = -1.0f;
    ui_.collab.chat.clear();
    ui_.collab.unread = 0;
    ui_.collab.chatAvailable = false;
    ui_.collab.policy.chat = settings().getBoolValue("collabAllowChat", true); // back to the defaults
    ui_.collab.policy.voice = settings().getBoolValue("collabAllowVoice", true);
    ui_.collab.policy.video = settings().getBoolValue("collabAllowVideo", true);
    if (onCollabChatChanged) onCollabChatChanged();
    if (ui_.requestRepaint) ui_.requestRepaint();
}

// ------------------------------------------------------------------------------------------------ voice (COL-7)

void AppController::collabVoiceArrived(collab::AuthorId speaker, const std::string& rtp)
{
    if (voice_) voice_->receive(speaker, rtp);
}

void AppController::setCollabMic(bool on)
{
    if (! collab_ || ! inCollabSession()) return;
    const auto camera = collabCameraOn_;
    if (! on)
    {
        collabMicOn_ = false;
        collab_->setMediaState({false, camera});
        pollVoice();
        refreshCollabView();
        return;
    }
    if (! collab_->voiceAvailable()) { collabNotice(collab_->voiceUnavailableReason()); return; }
    auto enable = [this, camera] {
        if (! collab_ || ! inCollabSession()) return;
        if (engine_.inputChannelCount() == 0)
        {
            // The mic is an input of the audio device: turn the first one on when none is (Settings > Audio picks others).
            const auto error = engine_.enableRecordingInputs(1);
            if (error.isNotEmpty())
            {
                collabNotice("No microphone: " + error.toStdString());
                return;
            }
        }
        collabMicOn_ = true;
        collab_->setMediaState({true, camera});
        if (ui_.collab.pushToTalk) collabNotice("Your mic is on: hold the ` key to talk");
        pollVoice();
        refreshCollabView();
    };
    if (juce::SystemStats::getEnvironmentVariable("SONORA_COLLAB_VOICE_TONE", {}).getFloatValue() > 0.0f) { enable(); return; } // autotest
    platform::requestMediaAccess(platform::MediaKind::Microphone, [this, enable, alive = std::weak_ptr<bool>(alive_)](bool granted) {
        if (alive.expired()) return;
        if (! granted)
        {
            collabNotice("Sonora may not use the microphone. Allow it in System Settings > Privacy & Security > Microphone.");
            return;
        }
        enable();
    });
}

void AppController::pollVoice()
{
    const bool wanted = collab_ && collab_->state() == collab::CollabSession::State::Active && collab_->voiceAvailable();
    if (! wanted)
    {
        voice_.reset();
        return;
    }
    if (! voice_)
    {
        voice_ = std::make_unique<collab::VoiceChannel>(engine_.voiceTap(), collab_->self());
        for (const auto who : mutedVoices_) voice_->setMuted(who, true);
    }
    voice_->setSelf(collab_->self());
    voice_->setTransmit(collabMicOn_ && (! ui_.collab.pushToTalk || pushToTalkHeld_));
    engine_.voiceTap().duckGain = ui_.collab.duck ? 0.5f : 1.0f;
    for (const auto& packet : voice_->takeOutgoing()) collab_->sendVoice(packet);
}

// ------------------------------------------------------------------------------------------------ camera (COL-8)

void AppController::collabVideoArrived(collab::AuthorId speaker, const std::string& rtp)
{
    if (video_) video_->receive(speaker, rtp);
}

void AppController::setCollabCamera(bool on)
{
    if (! collab_ || ! inCollabSession()) return;
    if (! on)
    {
        collabCameraOn_ = false;
        if (video_) video_->stopCamera();
        collab_->setMediaState({collabMicOn_, false});
        refreshCollabView();
        return;
    }
    if (! collab_->videoAvailable()) { collabNotice(collab_->videoUnavailableReason()); return; }
    const bool pattern = juce::SystemStats::getEnvironmentVariable("SONORA_COLLAB_VIDEO_TEST", {}).getIntValue() > 0; // autotests
    auto start = [this, pattern] {
        if (! collab_ || ! inCollabSession()) return;
        pollVideo();
        if (! video_ || ! video_->startCamera(pattern))
        {
            collabNotice("No camera could be opened.");
            return;
        }
        collabCameraOn_ = true;
        collab_->setMediaState({collabMicOn_, true});
        refreshCollabView();
    };
    if (pattern) { start(); return; }
    platform::requestMediaAccess(platform::MediaKind::Camera, [this, start, alive = std::weak_ptr<bool>(alive_)](bool granted) {
        if (alive.expired()) return;
        if (! granted)
        {
            collabNotice("Sonora may not use the camera. Allow it in System Settings > Privacy & Security > Camera.");
            return;
        }
        start();
    });
}

void AppController::pollVideo()
{
    const bool wanted = collab_ && collab_->state() == collab::CollabSession::State::Active && collab_->videoAvailable();
    if (! wanted)
    {
        if (video_)
        {
            video_.reset(); // switched off by the host, or the link went through TURN: the camera stops too
            if (collabCameraOn_ && collab_) collabNotice(collab_->videoUnavailableReason());
            collabCameraOn_ = false;
            ui_.collab.videoTiles.clear();
            if (collab_ && collab_->state() == collab::CollabSession::State::Active) collab_->setMediaState({collabMicOn_, false});
        }
        return;
    }
    if (! video_) video_ = std::make_unique<collab::VideoChannel>(collab_->self());
    video_->setSelf(collab_->self());
    video_->setSending(collabCameraOn_);
    for (const auto& packet : video_->takeOutgoing()) collab_->sendVideo(packet);
}

void AppController::refreshCollabVideo()
{
    auto& tiles = ui_.collab.videoTiles;
    const size_t before = tiles.size();
    uint64_t changed = 0;
    tiles.clear();
    if (video_ && collab_)
    {
        const auto now = juce::Time::currentTimeMillis();
        for (const auto& tile : video_->tiles())
        {
            const bool self = tile.author == collab_->self();
            const auto* p = collab_->participant(tile.author);
            if (! p || ! tile.image) continue;
            if (! self && (! collab_->mediaState(tile.author).cameraOn || now - tile.updatedMs > 3000)) continue; // they stopped
            if (self && ! collabCameraOn_) continue;
            ui::CollabView::VideoTile v;
            v.id = (int) tile.author;
            v.name = p->name;
            v.color = p->color;
            v.self = self;
            v.micOn = self ? collabMicOn_ : collab_->mediaState(tile.author).micOn;
            v.talking = voice_ && (self ? voice_->micTalking() : voice_->talking(tile.author));
            v.width = tile.image->width;
            v.height = tile.image->height;
            v.pixels = std::shared_ptr<const std::vector<uint32_t>>(tile.image, &tile.image->bgra); // shares the decoded frame
            v.serial = tile.serial;
            changed += tile.serial;
            tiles.push_back(std::move(v));
        }
    }
    if ((changed != videoSerial_ || tiles.size() != before) && ui_.requestRepaint) ui_.requestRepaint();
    videoSerial_ = changed;
}

std::string AppController::sendCollabChat(const std::string& text)
{
    if (! collab_) return "Not in a session.";
    return collab_->sendChat(text);
}

void AppController::setCollabChatOpen(bool open)
{
    ui_.collabChatOpen = open;
    if (open) ui_.collab.unread = 0;
    if (ui_.requestRepaint) ui_.requestRepaint();
}

void AppController::collabChatArrived(const collab::ChatEntry& e)
{
    if (collabAutotest_) std::fprintf(stderr, "[sonora] collab autotest: chat from %s: %s\n", e.name.c_str(), e.text.c_str());
    if (ui_.collabChatOpen) return;
    ++ui_.collab.unread;
    // Closed chat: the line shows in the status bar (first line, shortened) and the people button counts it.
    auto first = e.text.substr(0, e.text.find('\n'));
    if (first.size() > 80)
    {
        first.resize(80);
        while (! first.empty() && (static_cast<unsigned char>(first.back()) & 0xc0) == 0x80) first.pop_back();
        if (! first.empty() && (static_cast<unsigned char>(first.back()) & 0xc0) == 0xc0) first.pop_back();
        first += "...";
    }
    ui_.statusText = e.name + ": " + first;
}

// ------------------------------------------------------------------------------------------------ the joined set

void AppController::installCollabSong(std::unique_ptr<model::Song> song)
{
    if (! collab_) return;
    if (editingDevices_ || syncingModel_ || ui_.recording)
    {
        // installSong would defer it; try again shortly so the session learns when the set is in.
        auto shared = std::make_shared<std::unique_ptr<model::Song>>(std::move(song));
        juce::Timer::callAfterDelay(100, [this, alive = std::weak_ptr<bool>(alive_), shared] {
            if (! alive.expired() && *shared) installCollabSong(std::move(*shared));
        });
        return;
    }
    const juce::ScopedValueSetter<bool> installing(installingCollabSong_, true);
    installSong(std::move(song));
    collabPort_->guestReady = true;
    collab_->snapshotInstalled();
    ui_.statusText = "Joined " + collab_->sourceName();
    updateWindowTitle();
}

void AppController::collabAssetsArrived(const std::vector<std::string>& hashes)
{
    if (! song_ || ! collab_ || editingDevices_ || syncingModel_) return;
    // Clips reload their samples; sampler zones need their device rebuilt.
    std::set<std::string> arrived(hashes.begin(), hashes.end());
    const auto& catalog = collab_->assets();
    auto uses = [&](const model::FileRef& ref) {
        const auto entry = catalog.byKey(collab::assetKey(ref));
        return entry && arrived.count(entry->hash) > 0;
    };
    std::function<bool(const std::vector<model::Device>&)> devicesUse = [&](const std::vector<model::Device>& devices) {
        for (const auto& d : devices)
        {
            for (const auto& part : d.sampleParts)
                if (uses(part.sample)) return true;
            for (const auto& chain : d.chains)
                if (devicesUse(chain.devices)) return true;
        }
        return false;
    };
    std::vector<int> clipTracks, deviceTracks;
    for (int i = 0; i < ui_.numControlTracks(); ++i)
    {
        const auto* t = ui_.trackAt(i);
        if (! t) continue;
        bool clips = false;
        for (const auto& c : t->arrangementClips) clips = clips || (c.kind == model::Clip::Kind::Audio && uses(c.sample));
        for (const auto& s : t->slots) clips = clips || (s.clip && s.clip->kind == model::Clip::Kind::Audio && uses(s.clip->sample));
        for (const auto& l : t->takeLanes)
            for (const auto& c : l.clips) clips = clips || (c.kind == model::Clip::Kind::Audio && uses(c.sample));
        if (clips) clipTracks.push_back(i);
        if (devicesUse(t->devices)) deviceTracks.push_back(i);
    }
    for (int i : clipTracks) engine_.updateTrackClips(i, *song_);
    if (! deviceTracks.empty())
    {
        beginDeviceEdit();
        afterEdit(deviceTracks, false, true);
    }
    peaks_.clear();
    if (ui_.requestRepaint) ui_.requestRepaint();
}

// ------------------------------------------------------------------------------------------------ remote changes

int AppController::controlIndexOfTrack(int trackId) const
{
    if (! song_) return -1;
    for (size_t i = 0; i < song_->tracks.size(); ++i)
        if (song_->tracks[i].id == trackId) return (int) i;
    for (size_t i = 0; i < song_->returns.size(); ++i)
        if (song_->returns[i].id == trackId) return (int) (song_->tracks.size() + i);
    if (song_->main.id == trackId) return (int) (song_->tracks.size() + song_->returns.size());
    return -1;
}

void AppController::applyCollabObjects(const std::vector<collab::ObjectState>& incoming)
{
    if (! song_ || incoming.empty()) return;
    bool structure = false, devices = false;
    for (const auto& s : incoming)
    {
        structure = structure || s.key.kind == collab::ObjectKey::Kind::Structure
                 || (s.key.kind == collab::ObjectKey::Kind::Track && (! s.full || controlIndexOfTrack(s.key.id) < 0));
        devices = devices || s.key.kind == collab::ObjectKey::Kind::Devices;
    }
    // Live values stay as this machine has them (the live channel syncs them): capture them first.
    if (devices || structure) syncModelFromEngine();
    else if (auto controls = engine_.controls())
        for (int i = 0; i < ui_.numControlTracks() && (size_t) i < controls->size(); ++i)
            if (auto* t = const_cast<model::Track*>(ui_.trackAt(i)))
            {
                const auto& c = (*controls)[(size_t) i];
                t->mixer.volume = c.volume.load();
                t->mixer.pan = c.pan.load();
                t->mixer.speakerOn = c.active.load();
                t->mixer.solo = c.solo.load();
                for (size_t s = 0; s < t->mixer.sends.size() && s < c.sends.size(); ++s) t->mixer.sends[s] = c.sends[s].load();
            }
    auto states = incoming;
    collab::mergeLiveValues(*song_, states);
    if (structure) snapshotTrackUi();
    if (devices) beginDeviceEdit();
    collab::ApplyEffects fx;
    try { fx = collab::applyObjects(*song_, states); }
    catch (const model::CodecError& e)
    {
        std::fprintf(stderr, "[sonora] collab: could not apply objects: %s\n", e.what());
        if (devices) { editingDevices_ = false; ui_.song = song_.get(); }
        return;
    }
    history_.markExternalChange();
    std::vector<int> tracks;
    for (int i = 0; i < ui_.numControlTracks(); ++i)
        if (const auto* t = ui_.trackAt(i); t && (fx.headers.count(t->id) || fx.devices.count(t->id) || fx.clips.count(t->id)))
            tracks.push_back(i);
    afterEdit(tracks, fx.songHeader, devices, false, fx.structure || structure);
}

void AppController::applyLiveValues(const std::vector<collab::LiveValue>& values)
{
    auto* graph = engine_.latestGraph();
    auto controls = engine_.controls();
    if (! song_ || ! graph || ! controls || editingDevices_) return;
    for (const auto& v : values)
    {
        const int index = controlIndexOfTrack(v.trackId);
        if (index < 0 || (size_t) index >= controls->size() || (size_t) index >= graph->nodes.size()) continue;
        liveSent_[{v.trackId, v.deviceId, v.target}] = v.value;
        if (v.deviceId < 0)
        {
            auto& c = (*controls)[(size_t) graph->nodes[(size_t) index].controlIndex];
            if (v.target == collab::LiveValue::Volume) c.volume.store(std::clamp(v.value, 0.0f, 4.0f));
            else if (v.target == collab::LiveValue::Pan) c.pan.store(std::clamp(v.value, -1.0f, 1.0f));
            else if (v.target == collab::LiveValue::Active) c.active.store(v.value > 0.5f);
            else if (v.target == collab::LiveValue::Solo) c.solo.store(v.value > 0.5f);
            else if (v.target <= collab::LiveValue::SendBase)
            {
                const int send = collab::LiveValue::SendBase - v.target;
                if (send >= 0 && (size_t) send < c.sends.size()) c.sends[(size_t) send].store(std::clamp(v.value, 0.0f, 4.0f));
            }
            continue;
        }
        const auto* track = ui_.trackAt(index);
        if (! track) continue;
        forEachDeviceProcessor(track->devices, graph->nodes[(size_t) index].processorForModelDevice,
                               [&](const model::Device& d, devices::DeviceProcessor* p) {
                                   if (d.id != v.deviceId) return;
                                   if (v.target == collab::LiveValue::Bypass) p->setBypassed(v.value > 0.5f);
                                   else if (v.target >= 0 && (size_t) v.target < p->parameters().size())
                                       p->setParameter(v.target, std::clamp(v.value, p->parameters()[(size_t) v.target].minValue, p->parameters()[(size_t) v.target].maxValue));
                               });
    }
    if (ui_.requestRepaint) ui_.requestRepaint();
}

void AppController::pollLiveValues(bool plugins)
{
    auto* graph = engine_.latestGraph();
    auto controls = engine_.controls();
    if (! song_ || ! graph || ! controls || editingDevices_ || syncingModel_) return;
    std::vector<collab::LiveValue> out;
    auto check = [&](int track, int device, int target, float value) {
        const auto key = std::make_tuple(track, device, target);
        auto it = liveSent_.find(key);
        if (it == liveSent_.end()) { liveSent_.emplace(key, value); return; } // first sight: a baseline, not a change
        if (std::abs(it->second - value) <= 1.0e-6f) return;
        it->second = value;
        out.push_back({track, device, target, value});
    };
    for (int index = 0; index < ui_.numControlTracks() && (size_t) index < graph->nodes.size(); ++index)
    {
        const auto* track = ui_.trackAt(index);
        if (! track) continue;
        const auto& node = graph->nodes[(size_t) index];
        if ((size_t) node.controlIndex >= controls->size()) continue;
        const auto& c = (*controls)[(size_t) node.controlIndex];
        check(track->id, -1, collab::LiveValue::Volume, c.volume.load());
        check(track->id, -1, collab::LiveValue::Pan, c.pan.load());
        check(track->id, -1, collab::LiveValue::Active, c.active.load() ? 1.0f : 0.0f);
        check(track->id, -1, collab::LiveValue::Solo, c.solo.load() ? 1.0f : 0.0f);
        for (size_t s = 0; s < track->mixer.sends.size() && s < c.sends.size(); ++s)
            check(track->id, -1, collab::LiveValue::SendBase - (int) s, c.sends[s].load());
        forEachDeviceProcessor(track->devices, node.processorForModelDevice, [&](const model::Device& d, devices::DeviceProcessor* p) {
            if (d.id < 0) return;
            check(track->id, d.id, collab::LiveValue::Bypass, p->isBypassed() ? 1.0f : 0.0f);
            if (p->isHostedPlugin() && ! plugins) return; // large plugins are polled less often
            const auto count = std::min<size_t>(p->parameters().size(), 4096);
            for (size_t i = 0; i < count; ++i) check(track->id, d.id, (int) i, p->getParameter((int) i));
        });
    }
    if (! out.empty() && collab_) collab_->sendLive(std::move(out));
}

void AppController::pollPluginStates()
{
    // Changes made inside a plugin's own editor that are not parameters: compare the state of plugins with open editors.
    auto* graph = engine_.latestGraph();
    if (! song_ || ! graph || pluginWindows_.isEmpty() || editingDevices_ || syncingModel_ || ! collab_) return;
    std::vector<collab::PluginStateMsg> changed;
    {
        const juce::ScopedLock callbackLock(engine_.deviceManager().getAudioCallbackLock());
        for (int index = 0; index < ui_.numControlTracks() && (size_t) index < graph->nodes.size(); ++index)
        {
            const auto* track = ui_.trackAt(index);
            if (! track) continue;
            forEachDeviceProcessor(track->devices, graph->nodes[(size_t) index].processorForModelDevice,
                                   [&](const model::Device& d, devices::DeviceProcessor* p) {
                                       auto* plugin = dynamic_cast<plugins::PluginDevice*>(p);
                                       if (! plugin || ! d.plugin) return;
                                       if (! pluginEditorOpen(&plugin->instance())) return;
                                       auto copy = d;
                                       if (! plugin->captureState(copy) || ! copy.plugin) return;
                                       std::string bytes(copy.plugin->state.begin(), copy.plugin->state.end());
                                       bytes.append(copy.plugin->controllerState.begin(), copy.plugin->controllerState.end());
                                       const auto digest = crypto::sha256(bytes);
                                       uint64_t hash = 0;
                                       for (int i = 0; i < 8; ++i) hash = (hash << 8) | digest[(size_t) i];
                                       auto [it, inserted] = pluginStateHash_.emplace(d.id, hash);
                                       if (inserted || it->second == hash) return; // first sight is the baseline
                                       it->second = hash;
                                       collab::PluginStateMsg m;
                                       m.trackId = track->id;
                                       m.deviceId = d.id;
                                       m.state = copy.plugin->state;
                                       m.controllerState = copy.plugin->controllerState;
                                       changed.push_back(std::move(m));
                                   });
        }
    }
    for (auto& m : changed) collab_->sendPluginState(std::move(m));
}

void AppController::applyCollabPluginState(const collab::PluginStateMsg& m)
{
    if (! song_ || editingDevices_ || syncingModel_) return;
    const int index = controlIndexOfTrack(m.trackId);
    auto* track = const_cast<model::Track*>(ui_.trackAt(index));
    if (! track) return;
    std::function<model::Device*(std::vector<model::Device>&)> find = [&](std::vector<model::Device>& devices) -> model::Device* {
        for (auto& d : devices)
        {
            if (d.id == m.deviceId) return &d;
            for (auto& chain : d.chains)
                if (auto* found = find(chain.devices)) return found;
        }
        return nullptr;
    };
    auto* device = find(track->devices);
    if (! device || ! device->plugin) return;
    syncModelFromEngine();
    device = find(track->devices);
    if (! device || ! device->plugin) return;
    device->plugin->state = m.state;
    device->plugin->controllerState = m.controllerState;
    std::string bytes(m.state.begin(), m.state.end());
    bytes.append(m.controllerState.begin(), m.controllerState.end());
    const auto digest = crypto::sha256(bytes);
    uint64_t hash = 0;
    for (int i = 0; i < 8; ++i) hash = (hash << 8) | digest[(size_t) i];
    pluginStateHash_[m.deviceId] = hash;
    beginDeviceEdit();
    afterEdit({index}, false, true);
}

void AppController::applyCollabTransport(const collab::TransportMsg& m, int64_t localTimeMs)
{
    auto& transport = engine_.transport();
    using A = collab::TransportMsg::Action;
    if (m.action == A::Stop)
    {
        transport.stopNow();
        return;
    }
    const auto delay = static_cast<int>(std::clamp<int64_t>(localTimeMs - juce::Time::currentTimeMillis(), 0, 2000));
    auto act = [this, m] {
        auto& t = engine_.transport();
        if (m.beat >= 0.0) t.seekNow(m.beat);
        if (m.action == A::Play && ! t.isPlaying()) t.playNow();
        if (ui_.requestRepaint) ui_.requestRepaint();
    };
    if (delay <= 1) act();
    else juce::Timer::callAfterDelay(delay, [alive = std::weak_ptr<bool>(alive_), act] { if (! alive.expired()) act(); });
}

std::string AppController::performCollabDrop(collab::AuthorId author, const juce::File& file, const collab::DropRequestMsg& m)
{
    if (! song_) return {};
    const int index = m.trackId < 0 ? -1 : controlIndexOfTrack(m.trackId);
    if (m.trackId >= 0 && index < 0) return {};
    ui::BrowserDrag drag;
    drag.kind = ui::BrowserDrag::Kind::Sample;
    drag.value = file.getFullPathName().toStdString();
    drag.label = file.getFileName().toStdString();
    collabAuthorForNextEdit_ = author;
    collabAttributionSetMs_ = juce::Time::getMillisecondCounter();
    dropBrowserItem(drag, index, m.beat, m.scene);
    return "Drop " + drag.label;
}

// ------------------------------------------------------------------------------------------------ polling and the view

void AppController::pollCollab()
{
    if (! collab_)
    {
        if (ui_.collabOpen && ! lanBrowser_ && collab::networkAvailable()) lanBrowser_ = std::make_unique<collab::LanBrowser>();
        if (! ui_.collabOpen && lanBrowser_) lanBrowser_.reset();
        if (lanBrowser_ && ++collabTicks_ % 50 == 0) refreshCollabView();
        return;
    }
    collab_->poll();
    if (! collab_) return;
    pollVoice();
    pollVideo();
    if (video_ && collabTicks_ % 4 == 0) refreshCollabVideo(); // ~25 frames a second at most
    if (collabAuthorForNextEdit_ != collab::hostAuthor && juce::Time::getMillisecondCounter() - collabAttributionSetMs_ > 10000)
        collabAuthorForNextEdit_ = collab::hostAuthor;
    if (collab_->state() == collab::CollabSession::State::Active)
    {
        ++collabTicks_;
        if (collabTicks_ % 3 == 0) pollLiveValues(collabTicks_ % 9 == 0);
        if (collabTicks_ % 200 == 0) pollPluginStates();
        if (collabTicks_ % 10 == 0 && (! ui_.collab.cursors.empty() || voice_)) refreshCollabView(); // fade cursors; who talks
    }
}

void AppController::refreshCollabView()
{
    auto& v = ui_.collab;
    v.networkAvailable = collab::networkAvailable();
    if (lanBrowser_)
    {
        v.lanSessions.clear();
        for (const auto& s : lanBrowser_->sessions())
            v.lanSessions.push_back({(s.hostName.empty() ? s.service : s.hostName) + (s.sessionName.empty() ? "" : ": " + s.sessionName), s.address});
    }
    if (! collab_)
    {
        v.state = ui::CollabView::State::None;
        if (ui_.requestRepaint) ui_.requestRepaint();
        return;
    }
    v.state = viewState(collab_->state());
    v.host = collab_->isHost();
    v.canEdit = collab_->isHost() || collab_->canEdit();
    v.status = collab_->status();
    v.progress = collab_->progress();
    v.invite = collab_->inviteLink();
    if (copyInviteWhenReady_ && ! v.invite.empty())
    {
        // One click: the link is on the clipboard by the time the relay has registered the session.
        copyInviteWhenReady_ = false;
        juce::SystemClipboard::copyTextToClipboard(juce::String::fromUTF8(v.invite.c_str()));
        collabNotice("Invite link copied. Paste it to the people you want to work with.");
    }
    if (! v.host && ! collab_->sourceName().empty()) v.sessionName = juce::String::fromUTF8(collab_->sourceName().c_str()).upToLastOccurrenceOf(".", false, false).toStdString(); // a bare name: juce::File asserts
    v.participants.clear();
    for (const auto& p : collab_->participants())
    {
        ui::CollabView::Participant row;
        row.id = (int) p.id;
        row.name = p.name;
        row.role = viewRole(p.role);
        row.color = p.color;
        row.self = p.id == collab_->self();
        if (! row.self && (v.host || p.id == collab::hostAuthor)) row.safetyCode = collab_->safetyCode(p.id);
        row.hasChat = row.self || (collab_->featuresOf(p.id) & collab::FeatureChat) != 0;
        row.micOn = row.self ? collabMicOn_ : collab_->mediaState(p.id).micOn;
        row.talking = voice_ && (row.self ? voice_->micTalking() : voice_->talking(p.id));
        row.mutedHere = mutedVoices_.count(p.id) > 0;
        row.cameraOn = row.self ? collabCameraOn_ : collab_->mediaState(p.id).cameraOn;
        v.participants.push_back(row);
    }
    v.joinRequests = collab_->joinRequests();
    const auto& policy = collab_->policy();
    v.policy = {policy.chat, policy.voice, policy.video};
    v.chatAvailable = collab_->chatAvailable();
    v.voiceAvailable = collab_->voiceAvailable();
    v.voiceUnavailable = collab_->voiceUnavailableReason();
    v.micOn = collabMicOn_;
    v.talking = voice_ && voice_->micTalking();
    v.micLevel = voice_ && collabMicOn_ ? voice_->micLevel() : 0.0f;
    v.noMicInput = voice_ && collabMicOn_ && ! engine_.voiceTap().hasInput();
    v.videoAvailable = collab_->videoAvailable();
    v.videoUnavailable = collab_->videoUnavailableReason();
    v.cameraOn = collabCameraOn_;
    v.chatUnavailable = collab_->chatUnavailableReason();
    v.chat.clear();
    for (const auto& e : collab_->chat())
    {
        ui::CollabView::ChatLine line;
        line.author = (int) e.author;
        line.name = e.name;
        line.color = e.color;
        line.text = e.text;
        line.timeMs = e.timeMs;
        line.self = e.author == collab_->self();
        line.pending = line.self && e.id == 0;
        line.history = e.history;
        line.failed = e.failed;
        v.chat.push_back(std::move(line));
    }
    if (onCollabChatChanged) onCollabChatChanged();
    v.cursors.clear();
    const auto now = juce::Time::currentTimeMillis();
    for (const auto& [author, remote] : collab_->presence())
    {
        const auto* p = collab_->participant(author);
        if (! p) continue;
        const auto& m = remote.presence;
        ui::CollabView::Cursor c;
        c.id = (int) author;
        c.name = p->name;
        c.color = p->color;
        c.view = (int) m.view;
        c.trackId = m.trackId;
        c.beat = m.beat;
        c.pitch = m.pitch;
        c.clipUid = m.clipUid;
        c.selectedClipUid = m.selectedClipUid;
        c.selectedTrackFrom = m.selectedTrackFrom;
        c.selectedTrackTo = m.selectedTrackTo;
        c.selectionStart = m.selectionStart;
        c.selectionEnd = m.selectionEnd;
        c.age = (float) (now - remote.receivedMs) / 1000.0f;
        if (c.age < 30.0f) v.cursors.push_back(c);
    }
    if (ui_.requestRepaint) ui_.requestRepaint();
}

void AppController::collabNotice(const std::string& text)
{
    if (text.empty()) return;
    ui_.statusText = text;
    auto& notices = ui_.collab.notices;
    notices.push_back(text);
    while (notices.size() > 6) notices.erase(notices.begin());
    if (ui_.requestRepaint) ui_.requestRepaint();
}

// ------------------------------------------------------------------------------------------------ undo, saving

bool AppController::canUndo() const
{
    if (inCollabSession()) return collab_->state() == collab::CollabSession::State::Active && (collab_->isHost() ? ! collab_->undoLabel().empty() : true);
    return history_.canUndo();
}

bool AppController::canRedo() const
{
    if (inCollabSession()) return collab_->state() == collab::CollabSession::State::Active && (collab_->isHost() ? ! collab_->redoLabel().empty() : true);
    return history_.canRedo();
}

juce::String AppController::undoLabel() const
{
    if (inCollabSession()) return juce::String::fromUTF8(collab_->undoLabel().c_str());
    return history_.undoLabel();
}

juce::String AppController::redoLabel() const
{
    if (inCollabSession()) return juce::String::fromUTF8(collab_->redoLabel().c_str());
    return history_.redoLabel();
}

void AppController::saveCollabCopy()
{
    // A guest saves a copy of the shared set: the host's saved file is the template, and every sample the session
    // fetched is copied next to it so the copy opens anywhere.
    if (! song_ || ! collab_) return;
    if (collab_->sourceFile().empty()) { collabNotice("The host's set file hasn't arrived; can't save a copy yet."); return; }
    const int generation = documentGeneration_;
    const auto suggested = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                               .getChildFile(juce::String::fromUTF8(collab_->sourceName().c_str())).withFileExtension("als");
    chooser_ = std::make_unique<juce::FileChooser>("Save a Copy of the Session", suggested, "*.als");
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, generation](const juce::FileChooser& fc) {
            if (! song_ || ! collab_ || generation != documentGeneration_) return;
            auto destination = fc.getResult();
            if (destination == juce::File()) return;
            if (! destination.hasFileExtension("als")) destination = destination.withFileExtension("als");
            if (editingDevices_ || syncingModel_) { ui_.statusText = "Wait until the device operation has finished"; return; }
            syncModelFromEngine();
            auto copy = *song_;
            ui::captureSetView(ui_, copy);
            // The template the writer patches: the host's file as it was sent.
            const auto source = collab::collabCacheDirectory().getChildFile("sets").getChildFile(
                juce::String(crypto::toHex(crypto::sha256(collab_->sourceFile()))) + ".als");
            source.getParentDirectory().createDirectory();
            if (! source.existsAsFile() && ! source.replaceWithData(collab_->sourceFile().data(), collab_->sourceFile().size()))
            { ui_.statusText = "Could not save a copy: the cache is not writable"; return; }
            copy.sourcePath = source.getFullPathName().toStdString();
            const auto samples = destination.getParentDirectory().getChildFile("Samples").getChildFile("Imported");
            int copied = 0, missing = 0;
            std::map<std::string, std::string> done;
            auto relink = [&](model::FileRef& ref) {
                if (ref.absolutePath.empty() && ref.relativePath.empty()) return;
                const auto key = collab::assetKey(ref);
                if (auto it = done.find(key); it != done.end()) { ref.absolutePath = it->second; ref.relativePath.clear(); ref.sourceDirectory.clear(); return; }
                const auto cached = collab_->assets().resolve(ref);
                const auto entry = collab_->assets().byKey(key);
                if (cached.empty() || ! entry) { ++missing; return; }
                samples.createDirectory();
                const auto name = juce::File(juce::String::fromUTF8((ref.absolutePath.empty() ? ref.relativePath : ref.absolutePath).c_str())).getFileName();
                auto target = samples.getChildFile(juce::String::fromUTF8(collab::safeFileName(name.toStdString()).c_str()));
                if (target.existsAsFile() && target.getSize() != (juce::int64) entry->size) target = target.getNonexistentSibling(false);
                if (! target.existsAsFile()) juce::File(juce::String::fromUTF8(cached.c_str())).copyFileTo(target);
                ++copied;
                ref.absolutePath = target.getFullPathName().toStdString();
                ref.relativePath.clear();
                ref.sourceDirectory.clear();
                done[key] = ref.absolutePath;
            };
            auto relinkClip = [&](model::Clip& c) { if (c.kind == model::Clip::Kind::Audio) relink(c.sample); };
            std::function<void(std::vector<model::Device>&)> relinkDevices = [&](std::vector<model::Device>& devices) {
                for (auto& d : devices)
                {
                    for (auto& part : d.sampleParts) relink(part.sample);
                    for (auto& chain : d.chains) relinkDevices(chain.devices);
                }
            };
            auto relinkTrack = [&](model::Track& t) {
                for (auto& c : t.arrangementClips) relinkClip(c);
                for (auto& s : t.slots) if (s.clip) relinkClip(*s.clip);
                for (auto& l : t.takeLanes) for (auto& c : l.clips) relinkClip(c);
                for (auto& c : t.frozenArrangement) relinkClip(c);
                relinkDevices(t.devices);
            };
            for (auto& t : copy.tracks) relinkTrack(t);
            for (auto& t : copy.returns) relinkTrack(t);
            relinkTrack(copy.main);
            io::AlsSaveOptions options;
            options.portableReferences = true;
            const auto result = io::saveAls(copy, destination, options);
            ui_.statusText = result.ok ? "Saved a copy as " + destination.getFileName().toStdString() + " (" + std::to_string(copied) + " samples"
                                             + (missing ? ", " + std::to_string(missing) + " not downloaded" : std::string()) + ")"
                                       : "Could not save a copy: " + result.error;
            if (ui_.requestRepaint) ui_.requestRepaint();
        });
}

} // namespace sonora::app
