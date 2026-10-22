#pragma once
// SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 2104) for collab asset hashes and signalling authentication (COL-3/4).
// Plain C++, no dependency; tests check the standard vectors.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sonora::crypto
{

using Digest = std::array<uint8_t, 32>;

class Sha256
{
public:
    Sha256();
    void update(const void* data, size_t size);
    void update(std::string_view s) { update(s.data(), s.size()); }
    Digest finish();

private:
    void block(const uint8_t* p);
    std::array<uint32_t, 8> h_{};
    std::array<uint8_t, 64> buffer_{};
    size_t buffered_ = 0;
    uint64_t length_ = 0;
};

Digest sha256(std::string_view data);
Digest hmacSha256(std::string_view key, std::string_view message);

std::string toHex(const Digest&);
// base64url without padding (RFC 4648 section 5)
std::string base64Url(std::string_view bytes);
// Empty optional-like result: returns false on malformed input.
bool fromBase64Url(std::string_view text, std::string& out);
// Constant-time comparison of equal-length byte strings.
bool equalBytes(std::string_view a, std::string_view b);
// Cryptographically random bytes from the operating system.
std::string randomBytes(size_t n);

} // namespace sonora::crypto
