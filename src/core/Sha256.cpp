#include "core/Sha256.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#if defined(__APPLE__)
#include <Security/SecRandom.h>
#elif defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>
#endif

namespace sonora::crypto
{
namespace
{
constexpr uint32_t k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
} // namespace

Sha256::Sha256() : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const uint8_t* p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i)
    {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i)
    {
        const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, size_t size)
{
    const auto* p = static_cast<const uint8_t*>(data);
    length_ += size;
    while (size > 0)
    {
        if (buffered_ == 0 && size >= 64)
        {
            block(p);
            p += 64;
            size -= 64;
            continue;
        }
        const size_t n = std::min(size, 64 - buffered_);
        std::memcpy(buffer_.data() + buffered_, p, n);
        buffered_ += n;
        p += n;
        size -= n;
        if (buffered_ == 64)
        {
            block(buffer_.data());
            buffered_ = 0;
        }
    }
}

Digest Sha256::finish()
{
    const uint64_t bits = length_ * 8;
    const uint8_t one = 0x80, zero = 0;
    update(&one, 1);
    while (buffered_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(len, 8);
    Digest out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[(size_t) (4 * i + j)] = static_cast<uint8_t>(h_[(size_t) i] >> (24 - 8 * j));
    return out;
}

Digest sha256(std::string_view data)
{
    Sha256 s;
    s.update(data);
    return s.finish();
}

Digest hmacSha256(std::string_view key, std::string_view message)
{
    std::string k0(64, '\0');
    if (key.size() > 64)
    {
        const auto d = sha256(key);
        std::memcpy(k0.data(), d.data(), d.size());
    }
    else std::memcpy(k0.data(), key.data(), key.size());
    std::string ipad(64, '\0'), opad(64, '\0');
    for (size_t i = 0; i < 64; ++i)
    {
        ipad[i] = static_cast<char>(k0[i] ^ 0x36);
        opad[i] = static_cast<char>(k0[i] ^ 0x5c);
    }
    Sha256 inner;
    inner.update(ipad);
    inner.update(message);
    const auto innerDigest = inner.finish();
    Sha256 outer;
    outer.update(opad);
    outer.update(innerDigest.data(), innerDigest.size());
    return outer.finish();
}

std::string toHex(const Digest& d)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (const auto b : d)
    {
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 15]);
    }
    return s;
}

std::string base64Url(std::string_view bytes)
{
    std::string out;
    out.reserve((bytes.size() * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3)
    {
        const uint32_t v = (uint32_t(uint8_t(bytes[i])) << 16) | (uint32_t(uint8_t(bytes[i + 1])) << 8) | uint8_t(bytes[i + 2]);
        out.push_back(alphabet[(v >> 18) & 63]);
        out.push_back(alphabet[(v >> 12) & 63]);
        out.push_back(alphabet[(v >> 6) & 63]);
        out.push_back(alphabet[v & 63]);
    }
    if (bytes.size() - i == 1)
    {
        const uint32_t v = uint32_t(uint8_t(bytes[i])) << 16;
        out.push_back(alphabet[(v >> 18) & 63]);
        out.push_back(alphabet[(v >> 12) & 63]);
    }
    else if (bytes.size() - i == 2)
    {
        const uint32_t v = (uint32_t(uint8_t(bytes[i])) << 16) | (uint32_t(uint8_t(bytes[i + 1])) << 8);
        out.push_back(alphabet[(v >> 18) & 63]);
        out.push_back(alphabet[(v >> 12) & 63]);
        out.push_back(alphabet[(v >> 6) & 63]);
    }
    return out;
}

bool fromBase64Url(std::string_view text, std::string& out)
{
    out.clear();
    if (text.size() % 4 == 1) return false;
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : text)
    {
        const char* p = std::strchr(alphabet, c);
        if (c == '\0' || ! p) return false;
        acc = (acc << 6) | static_cast<uint32_t>(p - alphabet);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xff));
        }
    }
    return true;
}

bool equalBytes(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

std::string randomBytes(size_t n)
{
    std::string out(n, '\0');
    if (n == 0) return out;
#if defined(__APPLE__)
    if (SecRandomCopyBytes(kSecRandomDefault, n, out.data()) != errSecSuccess) throw std::runtime_error("no system randomness");
#elif defined(_WIN32)
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("no system randomness");
#else
    size_t done = 0;
    while (done < n)
    {
        const auto got = getrandom(out.data() + done, n - done, 0);
        if (got <= 0) throw std::runtime_error("no system randomness");
        done += static_cast<size_t>(got);
    }
#endif
    return out;
}

} // namespace sonora::crypto
