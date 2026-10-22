#pragma once
// COL-3: collab links over the network (WebRTC data channels through libdatachannel, DTLS end to end).
//
// Two ways to meet:
//  - Relay: the host registers a session with a relay (packages/collab-relay: hosted Sonora Cloud, self-hosted, or a
//    local development relay in open mode) and shares an invite link. The relay only forwards authenticated signalling
//    and hands out TURN credentials; edits and samples travel peer to peer (or as ciphertext through TURN).
//  - Local network: the host listens on a port and advertises itself with Bonjour. Guests join with the invite link,
//    or pick the session from the list and wait for the host to let them in.
//
// Built only with SONORA_COLLAB_NET (cmake/CollabNet.cmake); networkAvailable() says whether it is.

#include "collab/Invite.h"
#include "collab/Transport.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::collab
{

bool networkAvailable();

struct HostNetOptions
{
    enum class Mode
    {
        Relay,
        Lan,
    };
    Mode mode = Mode::Lan;
    std::string relay;        // origin of the relay, "https://relay.example" or "http://127.0.0.1:8787"
    std::string hostToken;    // account token for cloud/self-hosted relays ("" for open relays)
    int lanPort = 47100;      // 0: any free port
    std::string displayName;  // shown in other people's Join list (LAN)
    std::string sessionName;
    bool advertise = true;    // Bonjour (LAN)
};

std::unique_ptr<Transport> openHost(const HostNetOptions&);
std::unique_ptr<Transport> openGuest(const Invite&, const std::string& name);
// LAN without an invite: the host is asked to let `name` in.
std::unique_ptr<Transport> openGuestByAddress(const std::string& address, const std::string& name);

// Sessions advertised on the local network (Bonjour, macOS).
struct LanSession
{
    std::string service;     // Bonjour instance name
    std::string hostName;    // "Hana's Mac"
    std::string sessionName; // "Song.als"
    std::string address;     // "hanas-mac.local:47100"
};

class LanBrowser
{
public:
    LanBrowser();
    ~LanBrowser();
    LanBrowser(const LanBrowser&) = delete;
    LanBrowser& operator=(const LanBrowser&) = delete;
    std::vector<LanSession> sessions() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// This machine's LAN IPv4 addresses (for LAN invite links), most likely first.
std::vector<std::string> lanAddresses();

} // namespace sonora::collab
