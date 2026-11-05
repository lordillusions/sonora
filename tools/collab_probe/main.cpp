// collab_probe: a headless collab session over the real network (COL-3, docs/collab/PEER_PROTOCOL.md).
//
//   collab_probe <set.als|.xml> [--lan] [--relay URL [--token KEY] | --spawn-relay] [--guests N] [--edits N]
//   collab_probe <set.als|.xml> --chat-dock DIR      (macOS UI builds: PNGs of the native Session Chat dock)
//
// Runs a host and N guests in one process: WebRTC data channels through a relay (a local open-mode relay it starts
// itself with --spawn-relay, the default) and/or directly on the local network (--lan). It checks that every guest
// receives the set and its samples, that random concurrent edits from everyone converge, that per-user undo works,
// and that a wrong ticket, a wrong secret, garbage frames and an uninvited LAN guest are refused. Prints PASS/FAIL.
// --token is the host access key of a self-hosted relay (deploy/gcp.sh token). With SONORA_COLLAB_ICE_POLICY=relay
// every link must go through the relay's TURN server.

#include "collab/BarePort.h"
#include "collab/Net.h"
#include "collab/Video.h"
#include "core/AppDataFolder.h"
#include "core/ModelCodec.h"
#include "io/als/AlsReader.h"

#include <juce_core/juce_core.h>

#if SONORA_PROBE_CHAT_DOCK
#include "app/CollabChatDock.h"
#include "ui/UiContext.h"
#endif

#if SONORA_PROBE_SCTP_STATS
#include <usrsctp.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

using namespace sonora;

namespace
{
int failures = 0;

// COL-3c: time budgets that grow with the path. Over a far relay with forced TURN every message crosses the ocean twice
// (~400 ms) and a big set's join is bandwidth-bound, so LAN-sized budgets fail runs that work.
struct Budgets
{
    int64_t payloadBytes = 0; // samples + source file a guest downloads
    double relayRttMs = 0.0;  // HTTP round trip to the relay (0 on LAN)
    double peerRttMs = 0.0;   // measured after the join (clock pings)

    // SONORA_COLLAB_PROBE_JOIN_MS overrides. Far relays: two minutes plus the payload at 256 KiB/s per guest.
    int joinMs(int guests) const
    {
        const auto env = juce::SystemStats::getEnvironmentVariable("SONORA_COLLAB_PROBE_JOIN_MS", {}).getIntValue();
        if (env > 0) return env;
        if (relayRttMs < 60.0) return 120000;
        return 120000 + (int) std::min<int64_t>(payloadBytes * guests / 262, 1800000);
    }
    // A step that needs `trips` peer round trips, never less than the LAN budget.
    int stepMs(int lanMs, int trips) const { return std::max(lanMs, lanMs / 4 + (int) (trips * peerRttMs)); }
};

double relayRoundTripMs(const std::string& relay)
{
    double best = 0.0;
    for (int i = 0; i < 4; ++i) // the first request pays DNS and TLS
    {
        const auto t = juce::Time::getMillisecondCounterHiRes();
        int status = 0;
        auto stream = juce::URL(juce::String(relay) + "/v1/health")
                          .createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                                 .withConnectionTimeoutMs(5000)
                                                 .withStatusCode(&status));
        const auto ms = juce::Time::getMillisecondCounterHiRes() - t;
        if (stream && status == 200 && i > 0 && (best == 0.0 || ms < best)) best = ms;
    }
    return best;
}

void check(bool ok, const std::string& what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (! ok) ++failures;
}

std::string canonical(const model::Song& s)
{
    std::string out = model::codec::encodeDocument(s);
    for (const auto& k : collab::objectKeys(s)) out += collab::toString(k) + *collab::encodeObject(s, k);
    return out;
}

struct Replica
{
    std::unique_ptr<collab::BarePort> port = std::make_unique<collab::BarePort>();
    std::unique_ptr<collab::CollabSession> session;
};

struct Run
{
    Replica host;
    std::vector<Replica> guests;
    void pump(int ms)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
        while (juce::Time::getMillisecondCounter() < end)
        {
            host.session->poll();
            for (auto& g : guests) g.session->poll();
            juce::Thread::sleep(2);
        }
    }
    template <class F> bool pumpUntil(F&& done, int timeoutMs)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < end)
        {
            pump(5);
            if (done()) return true;
        }
        return done();
    }
    bool quiet()
    {
        for (auto& g : guests)
        {
            if (g.session->state() != collab::CollabSession::State::Active) return false;
            if (g.session->pendingEdits() != 0 || g.session->seq() != host.session->seq()) return false;
        }
        return true;
    }
};

bool randomEdit(model::Song& song, std::mt19937& random)
{
    if (song.tracks.empty()) return false;
    model::EditHistory history;
    const int track = (int) (random() % song.tracks.size());
    auto& t = song.tracks[(size_t) track];
    std::vector<int> uids;
    for (const auto& c : t.arrangementClips) uids.push_back(c.uid);
    const int uid = uids.empty() ? -1 : uids[random() % uids.size()];
    model::EditCommand cmd;
    switch (random() % 7)
    {
        case 0: cmd = model::setTempo(song, 90.0 + (double) (random() % 60)); break;
        case 1: cmd = model::renameTrack(song, track, "Probe " + std::to_string(random() % 1000)); break;
        case 2: cmd = model::setTrackColor(song, {track}, (int) (random() % 20)); break;
        case 3:
            if (uid >= 0)
                if (const auto* c = model::findArrangementClip(song, track, uid))
                    cmd = model::modifyClip(song, track, uid, model::movedClip(*c, c->currentStart + 4.0 * (double) (1 + random() % 4)), "Move Clip");
            break;
        case 4: if (uid >= 0 && uids.size() < 12) cmd = model::duplicateClip(song, track, uid); break;
        case 5: if (uid >= 0 && uids.size() > 2) cmd = model::deleteClip(song, track, uid); break;
        case 6: cmd = model::setLoop(song, random() % 2 == 0, 4.0 * (double) (random() % 8), 4.0 + 4.0 * (double) (random() % 4)); break;
    }
    if (! cmd.apply) return false;
    history.perform(song, std::move(cmd));
    return true;
}

std::unique_ptr<collab::CollabSession> makeGuest(collab::BarePort& port, std::unique_ptr<collab::Transport> transport, const std::string& name,
                                                 std::function<std::unique_ptr<collab::Transport>()> reconnect = {})
{
    collab::CollabSession::Options o;
    o.name = name;
    o.appVersion = "probe";
    auto s = collab::CollabSession::guest(port, std::move(transport), o, std::move(reconnect));
    port.session = s.get();
    return s;
}

void scenario(const char* title, const model::Song& set, const collab::HostNetOptions& net, int guests, int edits, Budgets budget)
{
    std::printf("\n== %s\n", title);
    if (net.mode == collab::HostNetOptions::Mode::Relay) budget.relayRttMs = relayRoundTripMs(net.relay);
    if (budget.relayRttMs > 0.0)
        std::printf("  relay round trip %.0f ms; join budget %d s\n", budget.relayRttMs, budget.joinMs(guests) / 1000);
    Run run;
    run.host.port->document = std::make_unique<model::Song>(set);
    collab::CollabSession::Options o;
    o.name = "Probe Host";
    o.sessionName = juce::File(set.sourcePath).getFileName().toStdString();
    auto hostTransport = collab::openHost(net);
    if (! hostTransport) { check(false, "collab network is compiled in"); return; }
    run.host.session = collab::CollabSession::host(*run.host.port, std::move(hostTransport), o);
    run.host.port->session = run.host.session.get();
    std::string link;
    run.pumpUntil([&] { link = run.host.session->inviteLink(); return ! link.empty() || run.host.port->ended_; }, 15000);
    check(! link.empty(), "host published an invite link (" + (link.empty() ? run.host.port->endReason : link.substr(0, link.find('?'))) + ")");
    if (link.empty()) return;
    std::string error;
    const auto invite = collab::parseInvite(link, error);
    check(invite.has_value(), "the invite link parses");
    if (! invite) return;

    const auto start = juce::Time::getMillisecondCounterHiRes();
    for (int g = 0; g < guests; ++g)
    {
        Replica r;
        const auto name = "Guest " + std::to_string(g + 1);
        r.session = makeGuest(*r.port, collab::openGuest(*invite, name), name, [again = *invite, name] { return collab::openGuest(again, name); });
        run.guests.push_back(std::move(r));
    }
    bool joined = run.pumpUntil([&] {
        bool ended = false;
        for (auto& g : run.guests) ended = ended || g.port->ended_;
        if (ended) return true;
        for (auto& g : run.guests)
            if (g.session->state() != collab::CollabSession::State::Active || g.session->downloadsPending() > 0) return false;
        return run.host.session->participants().size() == (size_t) guests + 1;
    }, budget.joinMs(guests));
    const double joinMs = juce::Time::getMillisecondCounterHiRes() - start;
    for (auto& g : run.guests) joined = joined && ! g.port->ended_;
    check(joined, std::to_string(guests) + " guests joined and fetched the samples in " + std::to_string((int) joinMs) + " ms");
    if (! joined)
    {
        for (auto& g : run.guests) std::printf("        guest state %d: %s\n", (int) g.session->state(), g.session->status().c_str());
        return;
    }
    bool same = true;
    for (auto& g : run.guests) same = same && canonical(*g.port->document) == canonical(*run.host.port->document);
    check(same, "every guest's set is identical to the host's");
    uint64_t assetBytes = 0;
    for (const auto& e : run.host.session->assets().entries()) assetBytes += e.size;
    const auto missing = run.guests[0].session->assets().missing().size();
    for (const auto& n : run.guests[0].port->notices) std::printf("        guest notice: %s\n", n.c_str());
    check(missing == 0, std::to_string(run.host.session->assets().entries().size()) + " sample references (" + std::to_string(assetBytes >> 10)
                            + " KiB) available to guests" + (missing ? ", " + std::to_string(missing) + " missing" : std::string()));
    bool codes = true;
    for (auto& g : run.guests)
        codes = codes && ! g.session->safetyCode(0).empty() && g.session->safetyCode(0) == run.host.session->safetyCode(g.session->self());
    check(codes, "host and guests derive the same safety code (" + run.guests[0].session->safetyCode(0) + ")");

    run.pump(2500); // a clock ping or two: the peer round trip sizes the budgets below
    for (auto& g : run.guests) budget.peerRttMs = std::max(budget.peerRttMs, (double) g.session->roundTripMs());
    if (budget.peerRttMs > 50.0) std::printf("  peer round trip %.0f ms: budgets scaled\n", budget.peerRttMs);

    // Concurrent random edits from everyone, then quiet.
    std::mt19937 random(42);
    int made = 0;
    const auto editStart = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < edits; ++i)
    {
        const size_t who = random() % (run.guests.size() + 1);
        auto& replica = who == 0 ? run.host : run.guests[who - 1];
        if (replica.port->document && randomEdit(*replica.port->document, random))
        {
            replica.session->localEdit("Probe Edit");
            ++made;
        }
        run.pump(3 + (int) (random() % 10));
    }
    const bool settled = run.pumpUntil([&] { return run.quiet(); }, budget.stepMs(20000, 40));
    const double editMs = juce::Time::getMillisecondCounterHiRes() - editStart;
    same = settled;
    for (auto& g : run.guests) same = same && canonical(*g.port->document) == canonical(*run.host.port->document);
    uint64_t rejects = 0;
    for (auto& g : run.guests) rejects += g.session->stats().rejects;
    check(same, std::to_string(made) + " concurrent edits converged (" + std::to_string(run.host.session->seq()) + " commits, "
                    + std::to_string(rejects) + " conflicts rolled back) in " + std::to_string((int) editMs) + " ms");
    const auto& hs = run.host.session->stats();
    std::printf("        host sent %llu KiB in all; %llu KiB of big bodies went as chunk references\n", (unsigned long long) (hs.bytesOut >> 10),
                (unsigned long long) (hs.referencedBytes >> 10));

    // Per-user undo round trip.
    auto& g0 = run.guests[0];
    const auto before = run.host.port->document->tempo;
    model::EditHistory h;
    h.perform(*g0.port->document, model::setTempo(*g0.port->document, 77.0));
    g0.session->localEdit("Set Tempo");
    run.pumpUntil([&] { return run.quiet() && std::abs(run.host.port->document->tempo - 77.0) < 1e-9; }, budget.stepMs(5000, 10));
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    g0.session->undo();
    const bool undone = run.pumpUntil([&] { return run.quiet() && std::abs(g0.port->document->tempo - before) < 1e-9; }, budget.stepMs(5000, 10));
    check(undone, "a guest's undo round trip took " + std::to_string((int) (juce::Time::getMillisecondCounterHiRes() - t0)) + " ms");

    // Presence and transport travel to everyone.
    collab::PresenceMsg p;
    p.beat = 12.0;
    p.trackId = run.host.port->document->tracks.empty() ? -1 : run.host.port->document->tracks[0].id;
    g0.session->sendPresence(p);
    run.guests.back().session->requestTransport(collab::TransportMsg::Action::Play, 0.0);
    const bool relayed = run.pumpUntil([&] {
        return run.host.session->presence().count(g0.session->self()) && ! run.host.port->transports.empty()
            && ! g0.port->transports.empty();
    }, budget.stepMs(5000, 10));
    check(relayed, "presence and a guest's Play reach the host and the other guests (clock offset "
                       + std::to_string(g0.session->clockOffsetMs()) + " ms, round trip " + std::to_string(g0.session->roundTripMs()) + " ms)");

    // COL-9 chat: every guest and the host say something; everyone reads everyone else's line, own lines are confirmed.
    {
        const double chatStart = juce::Time::getMillisecondCounterHiRes();
        bool sent = run.host.session->sendChat(juce::String::fromUTF8("host says hi \xf0\x9f\x8e\xb9").toStdString()).empty();
        for (size_t i = 0; i < run.guests.size(); ++i)
            sent = run.guests[i].session->sendChat("guest " + std::to_string(i) + " says hi").empty() && sent;
        const size_t lines = run.guests.size() + 1;
        auto confirmed = [](const collab::CollabSession& s) {
            return std::all_of(s.chat().begin(), s.chat().end(), [](const collab::ChatEntry& e) { return e.id != 0 && e.failed.empty(); });
        };
        const bool chatted = run.pumpUntil([&] {
            if (run.host.session->chat().size() != lines || run.host.port->chats.size() != lines - 1) return false;
            for (auto& g : run.guests)
                if (g.session->chat().size() != lines || g.port->chats.size() != lines - 1 || ! confirmed(*g.session)) return false;
            return true;
        }, budget.stepMs(5000, 10));
        // Others' lines arrive in the host's order; a guest's own line stays where it was typed.
        bool sameOrder = chatted;
        for (auto& g : run.guests)
        {
            std::vector<uint64_t> mine, host;
            for (const auto& e : g.session->chat()) if (e.author != g.session->self()) mine.push_back(e.id);
            for (const auto& e : run.host.session->chat()) if (e.author != g.session->self()) host.push_back(e.id);
            sameOrder = sameOrder && mine == host;
        }
        check(sent && chatted && sameOrder, "chat: " + std::to_string(lines) + " lines reached everyone in the host's order and were confirmed in "
                                   + std::to_string((int) (juce::Time::getMillisecondCounterHiRes() - chatStart)) + " ms");
        run.host.session->setPolicy({false, true, true});
        const bool off = run.pumpUntil([&] {
            return std::all_of(run.guests.begin(), run.guests.end(), [](const auto& g) { return ! g.session->chatAvailable(); });
        }, budget.stepMs(5000, 4));
        const bool refused = ! g0.session->sendChat("muted?").empty();
        run.host.session->setPolicy({true, true, true});
        const bool on = run.pumpUntil([&] { return g0.session->chatAvailable(); }, budget.stepMs(5000, 4));
        check(off && refused && on, "chat: the host turns chat off and on for everyone");
    }

    // COL-7 voice: RTP over each link's SRTP audio track; the host forwards guest 0's packets to the other guests.
    if (collab::voiceBuiltIn())
    {
        const bool ready = run.pumpUntil([&] {
            return run.host.session->voiceAvailable()
                && std::all_of(run.guests.begin(), run.guests.end(), [](const auto& g) { return g.session->voiceAvailable(); });
        }, budget.stepMs(5000, 4));
        check(ready, "voice: every link has an open audio track" + (ready ? std::string() : " (" + g0.session->voiceUnavailableReason() + ")"));
        if (ready)
        {
            run.host.port->voices.clear();
            for (auto& g : run.guests) g.port->voices.clear();
            const double voiceStart = juce::Time::getMillisecondCounterHiRes();
            constexpr int frames = 50; // one second of 20 ms frames
            for (int i = 0; i < frames; ++i)
            {
                collab::RtpPacket rtp;
                rtp.seq = (uint16_t) i;
                rtp.timestamp = (uint32_t) i * collab::voiceFrameSamples;
                rtp.ssrc = collab::voiceSsrc(g0.session->self());
                rtp.payload = std::string(60, (char) i); // the size of a 24 kbit/s Opus frame
                g0.session->sendVoice(collab::encodeRtp(rtp));
                run.pumpUntil([] { return false; }, 20);
            }
            auto heard = [&](collab::BarePort& port) {
                return (int) std::count_if(port.voices.begin(), port.voices.end(), [&](const auto& v) { return v.first == g0.session->self(); });
            };
            const bool arrived = run.pumpUntil([&] {
                bool all = heard(*run.host.port) >= frames * 9 / 10;
                for (size_t i = 1; i < run.guests.size(); ++i) all = all && heard(*run.guests[i].port) >= frames * 9 / 10;
                return all;
            }, budget.stepMs(3000, 4));
            std::string counts = "host " + std::to_string(heard(*run.host.port));
            for (size_t i = 1; i < run.guests.size(); ++i) counts += ", guest " + std::to_string(i) + " " + std::to_string(heard(*run.guests[i].port));
            check(arrived && g0.port->voices.empty(), "voice: a guest's 50 frames reached the host and were forwarded (" + counts + " in "
                                                          + std::to_string((int) (juce::Time::getMillisecondCounterHiRes() - voiceStart)) + " ms)");
        }
    }

    // COL-8 video: a guest's H.264 packets (synthetic, 30 frames of 3 KiB: FU-A) on the video track, forwarded by the host.
    if (collab::supportedFeatures & collab::FeatureVideo)
    {
        const bool ready = run.pumpUntil([&] { return g0.session->videoAvailable(); }, budget.stepMs(5000, 4));
        check(ready, "video: the links carry video" + (ready ? std::string() : " (" + g0.session->videoUnavailableReason() + ")"));
        if (ready)
        {
            const auto hostBefore = run.host.session->stats().videoIn;
            std::vector<uint64_t> guestsBefore;
            for (auto& g : run.guests) guestsBefore.push_back(g.session->stats().videoIn);
            uint16_t seq = 0;
            size_t sent = 0;
            for (int f = 0; f < 30; ++f)
            {
                std::string idr(3000, (char) f);
                idr[0] = 0x65;
                for (const auto& packet : collab::packetizeH264({idr}, collab::videoSsrc(g0.session->self()), seq, (uint32_t) f * 6000))
                {
                    g0.session->sendVideo(packet);
                    ++sent;
                }
                run.pumpUntil([] { return false; }, 30);
            }
            const bool arrived = run.pumpUntil([&] {
                bool all = run.host.session->stats().videoIn - hostBefore >= sent * 9 / 10;
                for (size_t i = 1; i < run.guests.size(); ++i) all = all && run.guests[i].session->stats().videoIn - guestsBefore[i] >= sent * 9 / 10;
                return all;
            }, budget.stepMs(3000, 4));
            std::string counts = "host " + std::to_string(run.host.session->stats().videoIn - hostBefore) + " of " + std::to_string(sent);
            for (size_t i = 1; i < run.guests.size(); ++i) counts += ", guest " + std::to_string(i) + " " + std::to_string(run.guests[i].session->stats().videoIn - guestsBefore[i]);
            check(arrived, "video: a guest's packets reached the host and were forwarded (" + counts + ")");
        }
    }

    // Refusals.
    if (net.mode != collab::HostNetOptions::Mode::Lan)
    {
        auto badTicket = *invite;
        badTicket.ticket = "wrong-ticket-value";
        Replica r1;
        r1.session = makeGuest(*r1.port, collab::openGuest(badTicket, "Intruder"), "Intruder");
        run.pumpUntil([&] { r1.session->poll(); return r1.port->ended_; }, 15000);
        check(r1.port->ended_ && run.host.session->participants().size() == (size_t) guests + 1,
              "a wrong ticket is refused by the relay (" + r1.port->endReason + ")");
    }
    else
    {
        Replica r1;
        r1.session = makeGuest(*r1.port, collab::openGuestByAddress(invite->lanAddress, "Uninvited"), "Uninvited");
        const bool asked = run.pumpUntil([&] { r1.session->poll(); return ! run.host.session->joinRequests().empty(); }, 10000);
        check(asked, "an uninvited LAN guest becomes a join request");
        if (asked) run.host.session->approveJoin(run.host.session->joinRequests().front().first, false);
        run.pumpUntil([&] { r1.session->poll(); return r1.port->ended_; }, 10000);
        check(r1.port->ended_ && run.host.session->participants().size() == (size_t) guests + 1,
              "a declined join request ends that guest's attempt (" + r1.port->endReason + ")");
    }
    {
        auto badSecret = *invite;
        badSecret.secret = std::string(16, 'x');
        Replica r2;
        r2.session = makeGuest(*r2.port, collab::openGuest(badSecret, "Forger"), "Forger");
        run.pumpUntil([&] { r2.session->poll(); return r2.port->ended_; }, 15000);
        check(r2.port->ended_ && run.host.session->participants().size() == (size_t) guests + 1,
              "a wrong invite secret never gets a connection (" + r2.port->endReason + ")");
    }
    run.pump(1500); // let the relay notice the refused guests' sockets closing (it holds at most four)
    {
        // A raw link that sends garbage after connecting is dropped by the host.
        auto raw = collab::openGuest(*invite, "Garbage");
        bool connected = false;
        std::string why;
        run.pumpUntil([&] {
            for (auto& e : raw->poll())
            {
                if (e.kind == collab::TransportEvent::Kind::Connected) connected = true;
                if (e.kind == collab::TransportEvent::Kind::Failed || e.kind == collab::TransportEvent::Kind::Closed) why = e.text;
            }
            return connected || ! why.empty();
        }, 15000);
        bool closed = false;
        if (connected)
        {
            raw->send(collab::hostPeer, collab::Channel::Ops, std::string("\xff\xff\xff garbage", 11));
            run.pumpUntil([&] {
                for (auto& e : raw->poll())
                    if (e.kind == collab::TransportEvent::Kind::Closed || e.kind == collab::TransportEvent::Kind::Failed) closed = true;
                return closed;
            }, 10000);
        }
        check(connected && closed, std::string("a peer that sends a malformed message is disconnected")
                                       + (connected ? (closed ? "" : " (connected, never closed)") : " (never connected: " + why + ")"));
    }

    // The host ends the session; everyone is told.
    run.host.session->leave();
    const bool ended = run.pumpUntil([&] {
        for (auto& g : run.guests)
            if (! g.port->ended_) return false;
        return true;
    }, budget.stepMs(10000, 10));
    check(ended, "ending the session reaches every guest (" + (run.guests.empty() ? std::string() : run.guests[0].port->endReason) + ")");
}

// --bench MB: raw data-channel throughput between one host and one guest transport, no session (COL-3c). The host
// pushes MB of 64 KiB messages on the Assets channel, keeping at most `backlogKb` queued (the session keeps 64), and
// pings over Ops every 250 ms: the pings show how long an edit waits behind bulk data. Stops after `seconds`.
void bench(const collab::HostNetOptions& net, int megabytes, int backlogKb, int seconds)
{
    std::printf("\n== Bench %s: %d MB on Assets, backlog %d KiB\n", net.mode == collab::HostNetOptions::Mode::Lan ? "LAN" : net.relay.c_str(),
                megabytes, backlogKb);
    auto host = collab::openHost(net);
    if (! host) { check(false, "collab network is compiled in"); return; }
    std::unique_ptr<collab::Transport> guest;
    collab::PeerId peer;
    bool guestUp = false;
    std::string why;
    auto now = [] { return juce::Time::getMillisecondCounterHiRes(); };
    std::vector<double> idlePings, loadPings, oneWay;
    uint64_t received = 0;
    double doneAt = 0.0;
    const uint64_t total = (uint64_t) megabytes << 20;
    auto ping = [&] {
        std::string m(9, 'P');
        const double t = now();
        std::memcpy(m.data() + 1, &t, 8);
        host->send(peer, collab::Channel::Ops, m);
    };
    auto drain = [&](bool loaded) {
        for (auto& e : host->poll())
        {
            if (e.kind == collab::TransportEvent::Kind::Connected) peer = e.peer;
            else if (e.kind == collab::TransportEvent::Kind::Closed || e.kind == collab::TransportEvent::Kind::Failed) why = "host: " + e.text;
            else if (e.kind == collab::TransportEvent::Kind::Message && e.data.size() == 9 && e.data[0] == 'Q')
            {
                double t = 0.0;
                std::memcpy(&t, e.data.data() + 1, 8);
                (loaded ? loadPings : idlePings).push_back(now() - t);
            }
            else if (e.kind == collab::TransportEvent::Kind::Message && e.data == "D") doneAt = now();
        }
        if (! guest) return;
        for (auto& e : guest->poll())
        {
            if (e.kind == collab::TransportEvent::Kind::Connected) guestUp = true;
            else if (e.kind == collab::TransportEvent::Kind::Closed || e.kind == collab::TransportEvent::Kind::Failed) why = "guest: " + e.text;
            else if (e.kind == collab::TransportEvent::Kind::Message && e.channel == collab::Channel::Ops && e.data.size() == 9)
            {
                double t = 0.0;
                std::memcpy(&t, e.data.data() + 1, 8);
                if (loaded) oneWay.push_back(now() - t);
                e.data[0] = 'Q';
                guest->send(collab::hostPeer, collab::Channel::Ops, e.data);
            }
            else if (e.kind == collab::TransportEvent::Kind::Message && e.channel == collab::Channel::Assets)
            {
                received += e.data.size();
                if (received == total) guest->send(collab::hostPeer, collab::Channel::Ops, "D");
            }
        }
    };
    auto until = [&](auto&& done, double ms, bool loaded) {
        const double end = now() + ms;
        while (now() < end && why.empty())
        {
            drain(loaded);
            if (done()) return true;
            juce::Thread::sleep(1);
        }
        return done();
    };
    std::string link;
    until([&] { link = host->inviteLink(); return ! link.empty(); }, 15000, false);
    std::string error;
    const auto invite = collab::parseInvite(link, error);
    if (! invite) { check(false, "invite link (" + why + ")"); return; }
    guest = collab::openGuest(*invite, "Bench");
    const bool up = until([&] { return guestUp && ! peer.empty(); }, 30000, false);
    check(up, "host and guest connected" + (why.empty() ? std::string() : " (" + why + ")"));
    if (! up) return;
    for (int i = 0; i < 5; ++i)
    {
        const auto n = idlePings.size();
        ping();
        until([&] { return idlePings.size() > n; }, 5000, false);
    }
#if SONORA_PROBE_SCTP_STATS
    sctpstat before{};
    usrsctp_get_stat(&before);
#endif
    const double start = now();
    const double reportMs = megabytes <= 64 && net.mode == collab::HostNetOptions::Mode::Lan ? 200.0 : 5000.0;
    double nextPing = start, nextReport = start + reportMs;
    uint64_t sent = 0;
    const std::string block(64 * 1024, 'x');
    const bool done = until([&] {
        while (sent < total && host->buffered(peer, collab::Channel::Assets) <= (size_t) backlogKb << 10)
        {
            host->send(peer, collab::Channel::Assets, block);
            sent += block.size();
        }
        if (now() >= nextPing) { ping(); nextPing += 250.0; }
        if (now() >= nextReport)
        {
            nextReport += reportMs;
            std::string counters;
#if SONORA_PROBE_SCTP_STATS
            sctpstat now_{};
            usrsctp_get_stat(&now_);
            counters = ", " + std::to_string(now_.sctps_sendretransdata - before.sctps_sendretransdata) + " retransmitted, "
                     + std::to_string(now_.sctps_timodata - before.sctps_timodata) + " T3";
#endif
            std::printf("    %4.0f s: %.1f MB received, %.2f MB/s, last Ops round trip %.0f ms, queued %zu KiB%s\n", (now() - start) / 1000.0,
                        (double) received / 1048576.0, (double) received / 1048576.0 / ((now() - start) / 1000.0),
                        loadPings.empty() ? 0.0 : loadPings.back(), host->buffered(peer, collab::Channel::Assets) >> 10, counters.c_str());
        }
        return doneAt > 0.0;
    }, seconds * 1000.0, true);
    const double elapsed = ((doneAt > 0.0 ? doneAt : now()) - start) / 1000.0;
    auto stats = [](std::vector<double> v) {
        if (v.empty()) return std::string("none");
        std::sort(v.begin(), v.end());
        char text[96];
        std::snprintf(text, sizeof(text), "min %.0f / median %.0f / max %.0f ms", v.front(), v[v.size() / 2], v.back());
        return std::string(text);
    };
#if SONORA_PROBE_SCTP_STATS
    sctpstat after{};
    usrsctp_get_stat(&after);
    std::printf("  SCTP (both ends): %u DATA chunks sent, %u retransmitted (%u fast), %u T3 timeouts, %u duplicate chunks received\n",
                after.sctps_senddata - before.sctps_senddata, after.sctps_sendretransdata - before.sctps_sendretransdata,
                after.sctps_sendfastretrans - before.sctps_sendfastretrans, after.sctps_timodata - before.sctps_timodata,
                after.sctps_recvdupdata - before.sctps_recvdupdata);
    std::printf("  SCTP detail: marked for retransmission %u, window probes %u, zero-window probes received %u, sack timers %u\n",
                after.sctps_markedretrans - before.sctps_markedretrans, after.sctps_timowindowprobe - before.sctps_timowindowprobe,
                after.sctps_recvdupdata - before.sctps_recvdupdata, after.sctps_timosack - before.sctps_timosack);
#endif
    std::printf("  idle Ops round trip: %s\n", stats(idlePings).c_str());
    std::printf("  Ops host -> guest under load: %s\n", stats(oneWay).c_str());
    std::printf("  Ops round trip under load (%zu pings): %s\n", loadPings.size(), stats(loadPings).c_str());
    check(done, "BENCH " + std::to_string(received >> 20) + " MB in " + std::to_string(elapsed).substr(0, 6) + " s = "
                    + std::to_string((double) received / 1048576.0 / elapsed).substr(0, 5) + " MB/s" + (why.empty() ? "" : " (" + why + ")"));
}

struct RelayProcess
{
    juce::ChildProcess process;
    std::string url;
    bool start(const juce::File& repo)
    {
        const auto relay = repo.getChildFile("packages/collab-relay/relay.mjs");
        if (! relay.existsAsFile() || ! repo.getChildFile("packages/collab-relay/node_modules/ws").isDirectory())
        {
            std::printf("  (no relay: run `npm ci --prefix packages/collab-relay`)\n");
            return false;
        }
        const int port = 20000 + juce::Random::getSystemRandom().nextInt(20000);
        juce::StringArray args{"/usr/bin/env", "SONORA_RELAY_MODE=open", "HOST=127.0.0.1", "PORT=" + juce::String(port), "node",
                               relay.getFullPathName()};
        if (! process.start(args, 0)) return false;
        url = "http://127.0.0.1:" + std::to_string(port);
        for (int i = 0; i < 100; ++i)
        {
            int status = 0;
            auto stream = juce::URL(juce::String(url) + "/v1/health")
                              .createInputStream(juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                                                     .withConnectionTimeoutMs(500)
                                                     .withStatusCode(&status));
            if (stream && status == 200) return true;
            juce::Thread::sleep(100);
        }
        return false;
    }
    ~RelayProcess() { process.kill(); }
};
} // namespace

#if SONORA_PROBE_CHAT_DOCK
// --chat-dock DIR (COL-9, ADR 0008): a host, a guest and an older guest chat over memory links; the guest's lines go
// into the native chat dock, which is captured as PNGs and checked for IME composition, Unicode, Return and
// Shift+Return. ui_snapshot cannot see native views.
static int chatDock(const juce::File& folder, const std::string& setPath)
{
    juce::ScopedJuceInitialiser_GUI gui;
    folder.createDirectory();
    auto loaded = io::loadAls(juce::File(juce::String::fromUTF8(setPath.c_str())));
    if (! loaded) { std::printf("FAIL: can't load %s\n", setPath.c_str()); return 1; }
    auto hub = collab::MemoryHub::create();
    Replica host, ana, old;
    host.port->document = std::make_unique<model::Song>(*loaded.song);
    collab::CollabSession::Options o;
    o.name = "Hana";
    o.sessionName = "Groove Sketch";
    host.session = collab::CollabSession::host(*host.port, hub->hostSide(), o);
    host.port->session = host.session.get();
    ana.session = makeGuest(*ana.port, hub->guestSide(), "Ana");
    collab::CollabSession::Options oldOptions;
    oldOptions.name = "Ben (0.1.9)";
    oldOptions.features = 0;
    old.session = collab::CollabSession::guest(*old.port, hub->guestSide(), oldOptions);
    old.port->session = old.session.get();
    std::unique_ptr<Replica> cleo; // joins after the first line: it arrives as history
    auto pump = [&](int rounds) {
        for (int i = 0; i < rounds; ++i)
        {
            host.session->poll();
            ana.session->poll();
            old.session->poll();
            if (cleo) cleo->session->poll();
            juce::Thread::sleep(1);
        }
    };
    pump(80);
    host.session->sendChat("Welcome! The bass is on track 2, have a listen.");
    pump(20);
    cleo = std::make_unique<Replica>();
    cleo->session = makeGuest(*cleo->port, hub->guestSide(), "Cleo");
    pump(80);
    ana.session->sendChat(juce::String::fromUTF8("Love it \xf0\x9f\x8e\xb9 Can we try the chorus an octave up?\nAlso: swing 56%?").toStdString());
    pump(20);
    cleo->session->sendChat(juce::String::fromUTF8("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e works too").toStdString());
    pump(20);
    host.session->setPolicy({false, true, true});
    cleo->session->sendChat("this one races the switch");
    pump(40);

    auto linesOf = [](const collab::CollabSession& s) {
        std::vector<app::CollabChatDock::Line> lines;
        for (const auto& e : s.chat())
        {
            app::CollabChatDock::Line l;
            l.name = e.name;
            l.text = e.text;
            l.time = juce::Time(e.timeMs).formatted("%H:%M").toStdString();
            l.color = ui::collabColor(e.color);
            l.self = e.author == s.self();
            l.pending = l.self && e.id == 0;
            l.history = e.history;
            l.failed = e.failed;
            lines.push_back(l);
        }
        return lines;
    };
    const bool oldOk = old.session->state() == collab::CollabSession::State::Active && old.session->chat().empty();
    check(oldOk, "the 0.1.9 guest stays connected and gets no chat");
    check(cleo->session->chat().size() == 4 && cleo->session->chat()[0].history && ! cleo->session->chat().back().failed.empty(),
          "Cleo sees the history, the new lines and her refused line");

    juce::DocumentWindow window("Session Chat verification", juce::Colours::darkgrey, juce::DocumentWindow::allButtons);
    auto* dock = new app::CollabChatDock();
    window.setUsingNativeTitleBar(true);
    window.setContentOwned(dock, false);
    window.centreWithSize(app::CollabChatDock::preferredWidth, 640);
    window.setVisible(true);
    bool captured = false, input = false;
    juce::Timer::callAfterDelay(400, [&] {
        dock->setSubtitle("Groove Sketch · 4 people");
        dock->setLines(linesOf(*cleo->session));
        dock->setComposer(cleo->session->chatAvailable(), cleo->session->chatUnavailableReason());
        captured = dock->writeSnapshot(folder.getChildFile("chat-dock-off.png"));
        host.session->setPolicy({true, true, true});
        pump(40);
        dock->setLines(linesOf(*ana.session));
        dock->setComposer(ana.session->chatAvailable(), ana.session->chatUnavailableReason());
        captured = dock->writeSnapshot(folder.getChildFile("chat-dock.png")) && captured;
        input = dock->verifyNativeInput();
        juce::MessageManager::getInstance()->stopDispatchLoop();
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    check(captured, "native chat dock captured: " + folder.getChildFile("chat-dock.png").getFullPathName().toStdString());
    check(input, "chat composer: marked text / Unicode / Return / Shift+Return");
    std::printf("\n%s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
#endif

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // progress shows up in logs even when a watchdog kills a slow run
    std::string setPath, relayUrl, token;
    bool lan = false, relay = false, spawn = false;
    int guests = 2, edits = 60, benchMb = 0, backlogKb = 1024, benchSeconds = 900;
    std::string chatDockDir;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--chat-dock" && i + 1 < argc) chatDockDir = argv[++i];
        else if (a == "--lan") lan = true;
        else if (a == "--relay" && i + 1 < argc) { relay = true; relayUrl = argv[++i]; }
        else if (a == "--token" && i + 1 < argc) token = argv[++i];
        else if (a == "--spawn-relay") { relay = spawn = true; }
        else if (a == "--guests" && i + 1 < argc) guests = std::max(1, std::min(3, std::atoi(argv[++i])));
        else if (a == "--edits" && i + 1 < argc) edits = std::max(0, std::atoi(argv[++i]));
        else if (a == "--bench" && i + 1 < argc) benchMb = std::max(1, std::atoi(argv[++i]));
        else if (a == "--backlog" && i + 1 < argc) backlogKb = std::max(0, std::atoi(argv[++i]));
        else if (a == "--seconds" && i + 1 < argc) benchSeconds = std::max(1, std::atoi(argv[++i]));
        else setPath = a;
    }
    if (setPath.empty() && benchMb == 0)
    {
        std::printf("usage: collab_probe <set.als|.xml> [--lan] [--relay URL [--token KEY] | --spawn-relay] [--guests N] [--edits N]\n"
                    "       collab_probe --bench MB [--backlog KB] [--seconds S] [--lan] [--relay URL [--token KEY] | --spawn-relay]\n");
        return 2;
    }
    if (! chatDockDir.empty())
    {
#if SONORA_PROBE_CHAT_DOCK
        return chatDock(juce::File(juce::String::fromUTF8(chatDockDir.c_str())), setPath);
#else
        std::printf("FAIL: --chat-dock needs a macOS build with the UI\n");
        return 1;
#endif
    }
    if (! lan && ! relay) lan = relay = spawn = true;
    if (! collab::networkAvailable())
    {
        std::printf("FAIL: built without SONORA_COLLAB_NET\n");
        return 1;
    }
    // Runs `body` on the local network and/or through the relay, as the options ask.
    auto eachMode = [&](auto&& body) {
        if (lan)
        {
            collab::HostNetOptions net;
            net.mode = collab::HostNetOptions::Mode::Lan;
            net.lanPort = 0;
            net.displayName = "Probe";
            net.advertise = false;
            body(net, "LAN (direct, no server)");
        }
        if (relay)
        {
            RelayProcess process;
            if (spawn && ! process.start(juce::File(SONORA_SOURCE_DIR))) check(false, "start a local open-mode relay");
            else
            {
                collab::HostNetOptions net;
                net.mode = collab::HostNetOptions::Mode::Relay;
                net.relay = spawn ? process.url : relayUrl;
                if (! spawn) net.hostToken = token;
                net.displayName = "Probe";
                body(net, "Relay " + net.relay);
            }
        }
    };
    if (benchMb > 0)
    {
        eachMode([&](const collab::HostNetOptions& net, const std::string&) { bench(net, benchMb, backlogKb, benchSeconds); });
        std::printf("\n%s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
        return failures == 0 ? 0 : 1;
    }
    auto loaded = io::loadAls(juce::File(juce::String::fromUTF8(setPath.c_str())));
    if (! loaded)
    {
        std::printf("FAIL: can't load %s: %s\n", setPath.c_str(), loaded.error.c_str());
        return 1;
    }
    std::printf("Set: %s (%zu tracks)\n", setPath.c_str(), loaded.song->tracks.size());
    {
        // What every local edit costs: finding the changed objects of the whole song.
        collab::ObjectIndex index;
        auto t = juce::Time::getMillisecondCounterHiRes();
        index.reset(*loaded.song);
        const double resetMs = juce::Time::getMillisecondCounterHiRes() - t;
        // Median of 21 scans: one-off stalls on a busy machine don't move it.
        std::vector<double> scans;
        for (int i = 0; i < 21; ++i)
        {
            t = juce::Time::getMillisecondCounterHiRes();
            (void) index.diff(*loaded.song);
            scans.push_back(juce::Time::getMillisecondCounterHiRes() - t);
        }
        std::sort(scans.begin(), scans.end());
        std::printf("Change detection: %zu objects, index %.1f ms, diff %.2f ms per edit (median of %zu, min %.2f)\n",
                    index.size(), resetMs, scans[scans.size() / 2], scans.size(), scans.front());
        // What a joining guest receives before the samples (COL-3c): the snapshot with its deflated body.
        collab::SnapshotMsg snap;
        size_t raw = 0;
        for (const auto& key : collab::objectKeys(*loaded.song))
        {
            snap.objects.push_back({key, index.full(key)});
            raw += snap.objects.back().full ? snap.objects.back().full->size() : 0;
        }
        snap.document = model::codec::encodeDocument(*loaded.song);
        raw += snap.document.size();
        juce::MemoryBlock source;
        if (juce::File(juce::String::fromUTF8(setPath.c_str())).loadFileAsData(source))
            snap.sourceFile.assign(static_cast<const char*>(source.getData()), source.getSize());
        t = juce::Time::getMillisecondCounterHiRes();
        collab::WireDictionary dictionary;
        const auto wire = collab::encodeMessage(snap, &dictionary);
        const double encodeMs = juce::Time::getMillisecondCounterHiRes() - t;
        t = juce::Time::getMillisecondCounterHiRes();
        collab::WireDictionary receiver;
        (void) collab::decodeMessage(wire, &receiver);
        std::printf("Snapshot: objects + document %zu KiB, source file %zu KiB, on the wire %zu KiB (encode %.0f ms, decode %.0f ms)\n",
                    raw >> 10, snap.sourceFile.size() >> 10, wire.size() >> 10, encodeMs, juce::Time::getMillisecondCounterHiRes() - t);
    }
    // Probe caches go to a private data folder: the real app's CollabCache stays untouched.
    sonora::core::setAppDataFolderName("Sonora Collab Probe");
    collab::collabCacheDirectory().deleteRecursively();
#if defined(_WIN32)
    _putenv_s("SONORA_COLLAB_CONNECT_TIMEOUT_MS", "8000");
#else
    ::setenv("SONORA_COLLAB_CONNECT_TIMEOUT_MS", "8000", 1);
#endif

    Budgets budget;
    budget.payloadBytes = juce::File(juce::String::fromUTF8(setPath.c_str())).getSize();
    for (const auto& ref : collab::sampleRefs(*loaded.song))
        budget.payloadBytes += juce::File(juce::String::fromUTF8(io::resolveProjectSamplePath(*loaded.song, ref).c_str())).getSize();
    eachMode([&](const collab::HostNetOptions& net, const std::string& title) { scenario(title.c_str(), *loaded.song, net, guests, edits, budget); });
    sonora::core::appDataDirectory().deleteRecursively(); // "Sonora Collab Probe" only
    std::printf("\n%s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
