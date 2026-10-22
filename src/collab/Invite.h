#pragma once
// COL-3: invite links, signalling authentication and the safety code (docs/collab/PEER_PROTOCOL.md "Security").
//
// An invite carries everything a guest needs. The secret sits in the URL fragment, which the app never sends
// anywhere: the relay sees the session id and the ticket hash only.
//
//   sonora://join/<sessionId>?relay=<origin>&ticket=<ticket>#<secret>     hosted or self-hosted relay
//   sonora://join/<sessionId>?lan=<host:port>&ticket=<ticket>#<secret>   direct, on the local network
//
// Signalling payloads (WebRTC offers, answers, candidates) are authenticated with HMAC-SHA256 keyed by the secret, so
// a relay or a stranger who learns the session id can't splice in its own DTLS certificate. Both peers then derive a
// four-word safety code from the secret and both certificate fingerprints, which people compare by voice once.

#include <optional>
#include <string>
#include <string_view>

namespace sonora::collab
{

struct Invite
{
    std::string sessionId;
    std::string relay;      // origin, "https://relay.example" (empty in LAN mode)
    std::string lanAddress; // "192.168.1.20:47100" (empty in relay mode)
    std::string ticket;     // base64url, given to the relay by guests
    std::string secret;     // raw bytes, never leaves the peers
    bool lan() const { return ! lanAddress.empty(); }
};

// New random ticket and secret (relay/lan and sessionId are filled by the caller).
Invite makeInvite();
std::string formatInvite(const Invite&);
// Accepts the link with or without surrounding whitespace; error explains a rejection.
std::optional<Invite> parseInvite(std::string_view text, std::string& error);

// SHA-256 of the ticket, lowercase hex: what the host registers with the relay.
std::string ticketHash(std::string_view ticket);

// Authentication of one signalling payload between two peers of a session.
std::string signalTag(std::string_view secret, std::string_view sessionId, std::string_view from, std::string_view to,
                      std::string_view payload);
bool verifySignal(std::string_view secret, std::string_view sessionId, std::string_view from, std::string_view to,
                  std::string_view payload, std::string_view tag);

// "amber-tiger-river-nine": the same on both ends when no one sits in the middle. Fingerprint order doesn't matter.
std::string safetyCode(std::string_view secret, std::string_view fingerprintA, std::string_view fingerprintB);

} // namespace sonora::collab
