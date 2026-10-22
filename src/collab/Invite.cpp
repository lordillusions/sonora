#include "collab/Invite.h"

#include "core/Sha256.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace sonora::collab
{
namespace
{
// 256 short, distinct, easy-to-say words: one byte each.
constexpr std::array<const char*, 256> words = {
    "acorn", "actor", "adobe", "agent", "alarm", "album", "alpha", "amber", "angle", "apple", "april", "arena", "arrow", "aspen",
    "atlas", "audio", "bacon", "badge", "bagel", "baker", "bamboo", "banjo", "basil", "beach", "bench", "berry", "bison", "blade",
    "blaze", "bloom", "board", "bonus", "brass", "bread", "brick", "bridge", "brook", "brush", "cabin", "cable", "cactus", "camel",
    "candy", "canoe", "cargo", "carol", "cedar", "chalk", "charm", "chess", "chief", "cider", "cinema", "circle", "civic", "clay",
    "cliff", "cloud", "clover", "coach", "cobalt", "comet", "coral", "cotton", "crane", "crown", "cumin", "cycle", "daisy", "dance",
    "delta", "denim", "desert", "diary", "diner", "disco", "dolphin", "donut", "dragon", "drum", "eagle", "earth", "echo", "elbow",
    "elder", "ember", "empire", "engine", "falcon", "fancy", "feast", "fern", "fiber", "field", "flame", "flint", "flute", "focus",
    "forest", "fossil", "fox", "frost", "fruit", "galaxy", "garden", "garlic", "gecko", "giant", "ginger", "glacier", "globe", "gold",
    "grape", "gravel", "guitar", "habit", "harbor", "hazel", "heron", "hollow", "honey", "horizon", "hotel", "husky", "igloo", "index",
    "indigo", "iris", "island", "ivory", "jacket", "jade", "jaguar", "jazz", "jelly", "jewel", "jungle", "kayak", "kettle", "kiwi",
    "koala", "ladder", "lagoon", "lemon", "lens", "lilac", "linen", "lion", "lotus", "lunar", "magnet", "mango", "maple", "marble",
    "meadow", "melon", "metro", "mint", "mirror", "mocha", "monkey", "moss", "motor", "nectar", "needle", "nickel", "noble", "north",
    "nova", "oasis", "ocean", "olive", "onion", "opera", "orbit", "orchid", "otter", "oxygen", "paddle", "panda", "paper", "parrot",
    "peach", "pearl", "pepper", "piano", "pilot", "pixel", "planet", "plum", "polar", "pony", "prism", "pumpkin", "quartz", "quill",
    "radar", "radio", "raven", "reef", "rhythm", "ribbon", "river", "robin", "rocket", "rose", "ruby", "saddle", "salmon", "sauce",
    "scout", "shadow", "shell", "silver", "sketch", "slate", "solar", "sonic", "spice", "spruce", "stone", "storm", "sugar", "summit",
    "swan", "tango", "temple", "thunder", "tiger", "timber", "tomato", "topaz", "torch", "tulip", "tundra", "turtle", "ultra", "umber",
    "unity", "urban", "valley", "velvet", "violet", "violin", "vivid", "walnut", "wave", "willow", "winter", "wizard", "yarn", "zebra",
    "zephyr", "zinc", "zone", "oak"};

std::string urlEncode(std::string_view s)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s)
    {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(ch);
        else
        {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

bool urlDecode(std::string_view s, std::string& out)
{
    out.clear();
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '%') { out.push_back(s[i] == '+' ? ' ' : s[i]); continue; }
        if (i + 2 >= s.size() || ! std::isxdigit(static_cast<unsigned char>(s[i + 1])) || ! std::isxdigit(static_cast<unsigned char>(s[i + 2])))
            return false;
        out.push_back(static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16)));
        i += 2;
    }
    return true;
}

bool safeToken(std::string_view s, size_t maxLength)
{
    return ! s.empty() && s.size() <= maxLength
        && std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
}

std::string field(std::string_view s)
{
    // Length-prefixed, so ("ab", "c") and ("a", "bc") authenticate differently.
    return std::to_string(s.size()) + ":" + std::string(s);
}
} // namespace

Invite makeInvite()
{
    Invite i;
    i.ticket = crypto::base64Url(crypto::randomBytes(32));
    i.secret = crypto::randomBytes(16);
    return i;
}

std::string formatInvite(const Invite& i)
{
    std::string s = "sonora://join/" + i.sessionId + "?";
    if (i.lan()) s += "lan=" + urlEncode(i.lanAddress);
    else s += "relay=" + urlEncode(i.relay);
    s += "&ticket=" + i.ticket + "#" + crypto::base64Url(i.secret);
    return s;
}

std::optional<Invite> parseInvite(std::string_view text, std::string& error)
{
    while (! text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (! text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    constexpr std::string_view prefix = "sonora://join/";
    if (text.substr(0, prefix.size()) != prefix) { error = "This is not a Sonora invite link (it starts with sonora://join/)."; return std::nullopt; }
    text.remove_prefix(prefix.size());
    const auto hash = text.find('#');
    if (hash == std::string_view::npos) { error = "The invite link is incomplete: the part after # is missing."; return std::nullopt; }
    const auto fragment = text.substr(hash + 1);
    text = text.substr(0, hash);
    const auto question = text.find('?');
    if (question == std::string_view::npos) { error = "The invite link is incomplete."; return std::nullopt; }
    Invite i;
    i.sessionId = std::string(text.substr(0, question));
    if (! safeToken(i.sessionId, 128)) { error = "The invite link has an invalid session id."; return std::nullopt; }
    auto query = text.substr(question + 1);
    while (! query.empty())
    {
        const auto amp = query.find('&');
        const auto pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos) continue;
        std::string value;
        if (! urlDecode(pair.substr(eq + 1), value)) { error = "The invite link is damaged."; return std::nullopt; }
        const auto key = pair.substr(0, eq);
        if (key == "relay") i.relay = value;
        else if (key == "lan") i.lanAddress = value;
        else if (key == "ticket") i.ticket = value;
    }
    if (! safeToken(i.ticket, 128)) { error = "The invite link has no valid ticket."; return std::nullopt; }
    if (i.relay.empty() == i.lanAddress.empty()) { error = "The invite link must name either a relay or a local address."; return std::nullopt; }
    if (! i.relay.empty())
    {
        const bool https = i.relay.rfind("https://", 0) == 0;
        const bool localHttp = i.relay.rfind("http://127.0.0.1", 0) == 0 || i.relay.rfind("http://localhost", 0) == 0;
        if (! https && ! localHttp) { error = "The relay in the invite must use https."; return std::nullopt; }
        while (! i.relay.empty() && i.relay.back() == '/') i.relay.pop_back();
    }
    if (! i.lanAddress.empty()
        && (i.lanAddress.size() > 64 || i.lanAddress.find_first_not_of("0123456789abcdefABCDEF.:[]-") != std::string::npos))
    {
        error = "The local address in the invite is invalid.";
        return std::nullopt;
    }
    if (! crypto::fromBase64Url(fragment, i.secret) || i.secret.size() < 16)
    {
        error = "The invite link's secret is damaged.";
        return std::nullopt;
    }
    return i;
}

std::string ticketHash(std::string_view ticket) { return crypto::toHex(crypto::sha256(ticket)); }

std::string signalTag(std::string_view secret, std::string_view sessionId, std::string_view from, std::string_view to,
                      std::string_view payload)
{
    const auto message = "sonora-signal-v1|" + field(sessionId) + field(from) + field(to) + field(payload);
    const auto mac = crypto::hmacSha256(secret, message);
    return crypto::base64Url({reinterpret_cast<const char*>(mac.data()), mac.size()});
}

bool verifySignal(std::string_view secret, std::string_view sessionId, std::string_view from, std::string_view to,
                  std::string_view payload, std::string_view tag)
{
    return crypto::equalBytes(signalTag(secret, sessionId, from, to, payload), tag);
}

std::string safetyCode(std::string_view secret, std::string_view fingerprintA, std::string_view fingerprintB)
{
    std::string a(fingerprintA), b(fingerprintB);
    for (auto* s : {&a, &b})
        std::transform(s->begin(), s->end(), s->begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (b < a) std::swap(a, b);
    const auto mac = crypto::hmacSha256(secret, "sonora-safety-v1|" + field(a) + field(b));
    std::string code;
    for (int i = 0; i < 4; ++i)
    {
        if (i) code += "-";
        code += words[mac[(size_t) i]];
    }
    return code;
}

} // namespace sonora::collab
