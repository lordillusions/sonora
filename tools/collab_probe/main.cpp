// collab_probe: a headless collab session over the real network (COL-3, docs/collab/PEER_PROTOCOL.md).
//
//   collab_probe <set.als|.xml> [--lan] [--relay URL | --spawn-relay] [--guests N] [--edits N]
//
// Runs a host and N guests in one process: WebRTC data channels through a relay (a local open-mode relay it starts
// itself with --spawn-relay, the default) and/or directly on the local network (--lan). It checks that every guest
// receives the set and its samples, that random concurrent edits from everyone converge, that per-user undo works,
// and that a wrong ticket, a wrong secret, garbage frames and an uninvited LAN guest are refused. Prints PASS/FAIL.

#include "collab/BarePort.h"
#include "collab/Net.h"
#include "core/AppDataFolder.h"
#include "core/ModelCodec.h"
#include "io/als/AlsReader.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace sonora;

namespace
{
int failures = 0;

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

void scenario(const char* title, const model::Song& set, const collab::HostNetOptions& net, int guests, int edits)
{
    std::printf("\n== %s\n", title);
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
    }, 120000);
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
    const bool settled = run.pumpUntil([&] { return run.quiet(); }, 20000);
    const double editMs = juce::Time::getMillisecondCounterHiRes() - editStart;
    same = settled;
    for (auto& g : run.guests) same = same && canonical(*g.port->document) == canonical(*run.host.port->document);
    uint64_t rejects = 0;
    for (auto& g : run.guests) rejects += g.session->stats().rejects;
    check(same, std::to_string(made) + " concurrent edits converged (" + std::to_string(run.host.session->seq()) + " commits, "
                    + std::to_string(rejects) + " conflicts rolled back) in " + std::to_string((int) editMs) + " ms");

    // Per-user undo round trip.
    auto& g0 = run.guests[0];
    const auto before = run.host.port->document->tempo;
    model::EditHistory h;
    h.perform(*g0.port->document, model::setTempo(*g0.port->document, 77.0));
    g0.session->localEdit("Set Tempo");
    run.pumpUntil([&] { return run.quiet() && std::abs(run.host.port->document->tempo - 77.0) < 1e-9; }, 5000);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    g0.session->undo();
    const bool undone = run.pumpUntil([&] { return run.quiet() && std::abs(g0.port->document->tempo - before) < 1e-9; }, 5000);
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
    }, 5000);
    check(relayed, "presence and a guest's Play reach the host and the other guests (clock offset "
                       + std::to_string(g0.session->clockOffsetMs()) + " ms, round trip " + std::to_string(g0.session->roundTripMs()) + " ms)");

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
    }, 10000);
    check(ended, "ending the session reaches every guest (" + (run.guests.empty() ? std::string() : run.guests[0].port->endReason) + ")");
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

int main(int argc, char** argv)
{
    std::string setPath, relayUrl;
    bool lan = false, relay = false, spawn = false;
    int guests = 2, edits = 60;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--lan") lan = true;
        else if (a == "--relay" && i + 1 < argc) { relay = true; relayUrl = argv[++i]; }
        else if (a == "--spawn-relay") { relay = spawn = true; }
        else if (a == "--guests" && i + 1 < argc) guests = std::max(1, std::min(3, std::atoi(argv[++i])));
        else if (a == "--edits" && i + 1 < argc) edits = std::max(0, std::atoi(argv[++i]));
        else setPath = a;
    }
    if (setPath.empty())
    {
        std::printf("usage: collab_probe <set.als|.xml> [--lan] [--relay URL | --spawn-relay] [--guests N] [--edits N]\n");
        return 2;
    }
    if (! lan && ! relay) lan = relay = spawn = true;
    if (! collab::networkAvailable())
    {
        std::printf("FAIL: built without SONORA_COLLAB_NET\n");
        return 1;
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
    }
    // Probe caches go to a private data folder: the real app's CollabCache stays untouched.
    sonora::core::setAppDataFolderName("Sonora Collab Probe");
    collab::collabCacheDirectory().deleteRecursively();
#if defined(_WIN32)
    _putenv_s("SONORA_COLLAB_CONNECT_TIMEOUT_MS", "8000");
#else
    ::setenv("SONORA_COLLAB_CONNECT_TIMEOUT_MS", "8000", 1);
#endif

    if (lan)
    {
        collab::HostNetOptions net;
        net.mode = collab::HostNetOptions::Mode::Lan;
        net.lanPort = 0;
        net.displayName = "Probe";
        net.advertise = false;
        scenario("LAN (direct, no server)", *loaded.song, net, guests, edits);
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
            net.displayName = "Probe";
            scenario(("Relay " + net.relay).c_str(), *loaded.song, net, guests, edits);
        }
    }
    sonora::core::appDataDirectory().deleteRecursively(); // "Sonora Collab Probe" only
    std::printf("\n%s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
